#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

#include "home_iot.h"
#include "ipc.h"

typedef struct {
    ipc_context_t *ipc;
    const runtime_config_t *config;
    pthread_mutex_t action_mutex;
    pthread_cond_t action_condition;
    iot_msg_t actions[MAX_RULES];
    int action_count;
    volatile sig_atomic_t stopping;
} watcher_state_t;

static void enqueue_action(watcher_state_t *state, const alert_rule_entry_t *rule)
{
    pthread_mutex_lock(&state->action_mutex);
    if (state->action_count < MAX_RULES) {
        iot_msg_t *message = &state->actions[state->action_count++];
        message->mtype = 1;
        message->console_id = 0;
        (void)snprintf(message->target, sizeof(message->target), "%s", rule->auto_actuator);
        (void)snprintf(message->payload, sizeof(message->payload), "%s",
                       rule->auto_action_on ? "ON" : "OFF");
        message->parameter = rule->auto_action_duration;
        pthread_cond_signal(&state->action_condition);
    }
    pthread_mutex_unlock(&state->action_mutex);
}

static void send_alert(const watcher_state_t *state, const alert_entry_t *alert, int severity)
{
    iot_msg_t message = {0};
    message.mtype = severity;
    message.console_id = 0;
    (void)snprintf(message.payload, sizeof(message.payload), "ALERT %s rule=%s value=%d\n",
                   alert->alert_id, alert->rule_id, alert->value);
    if (msgsnd(state->ipc->alerts_queue_id, &message, MSG_PAYLOAD_SIZE, IPC_NOWAIT) == -1 &&
        errno != EAGAIN) {
        return;
    }
}

static void evaluate_key(watcher_state_t *state, const key_telemetry_t *key, int timestamp)
{
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    for (int index = 0; index < state->ipc->alerts->total_rules; index++) {
        alert_rule_entry_t *rule = &state->ipc->alerts->rules[index];
        int violated = strcmp(rule->key, key->key) == 0 &&
                       (key->last_value < rule->min_limit || key->last_value > rule->max_limit);
        if (violated && !rule->is_active_alert) {
            alert_entry_t alert = {0};
            int alert_index = state->ipc->alerts->total_alerts < MAX_ALERTS
                                  ? state->ipc->alerts->total_alerts++
                                  : 0;
            (void)snprintf(alert.alert_id, sizeof(alert.alert_id), "AL_%03d",
                           state->ipc->alerts->next_alert_seq++);
            (void)snprintf(alert.rule_id, sizeof(alert.rule_id), "%s", rule->rule_id);
            alert.value = key->last_value;
            alert.timestamp_ut = timestamp;
            alert.state = ALERT_ACTIVE;
            state->ipc->alerts->alerts[alert_index] = alert;
            rule->is_active_alert = 1;
            pthread_mutex_lock(&state->ipc->global_stats->mutex);
            state->ipc->global_stats->total_alerts++;
            pthread_mutex_unlock(&state->ipc->global_stats->mutex);
            send_alert(state, &alert, rule->severity);
            if (rule->auto_actuator[0] != '\0') enqueue_action(state, rule);
        } else if (!violated && rule->is_active_alert) {
            rule->is_active_alert = 0;
            for (int alert_index = 0; alert_index < state->ipc->alerts->total_alerts; alert_index++) {
                alert_entry_t *alert = &state->ipc->alerts->alerts[alert_index];
                if (strcmp(alert->rule_id, rule->rule_id) == 0 && alert->state != ALERT_RESOLVED) {
                    alert->state = ALERT_RESOLVED;
                }
            }
        }
    }
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
}

static void *evaluator_thread(void *argument)
{
    watcher_state_t *state = argument;
    key_telemetry_t keys[MAX_KEYS];
    while (!state->stopping) {
        if (pthread_mutex_lock(&state->ipc->telemetry->mutex) != 0) break;
        while (!state->stopping && state->ipc->telemetry->update_seq == 0)
            pthread_cond_wait(&state->ipc->telemetry->cond_update, &state->ipc->telemetry->mutex);
        int total_keys = state->ipc->telemetry->total_keys;
        if (total_keys > MAX_KEYS) total_keys = MAX_KEYS;
        memcpy(keys, state->ipc->telemetry->keys, (size_t)total_keys * sizeof(keys[0]));
        state->ipc->telemetry->update_seq = 0;
        pthread_mutex_unlock(&state->ipc->telemetry->mutex);
        for (int index = 0; index < total_keys && !state->stopping; index++) {
            int timestamp = 0;
            if (pthread_mutex_lock(&state->ipc->global_stats->mutex) == 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long long elapsed = (long long)(now.tv_sec - state->ipc->global_stats->start_time.tv_sec) * 1000LL;
                elapsed += (now.tv_nsec - state->ipc->global_stats->start_time.tv_nsec) / 1000000LL;
                timestamp = (int)(elapsed / state->config->time_unit_ms);
                pthread_mutex_unlock(&state->ipc->global_stats->mutex);
            }
            evaluate_key(state, &keys[index], timestamp);
        }
    }
    return NULL;
}

static void *action_thread(void *argument)
{
    watcher_state_t *state = argument;
    while (!state->stopping) {
        pthread_mutex_lock(&state->action_mutex);
        while (state->action_count == 0 && !state->stopping)
            pthread_cond_wait(&state->action_condition, &state->action_mutex);
        if (state->action_count > 0) {
            iot_msg_t message = state->actions[0];
            memmove(&state->actions[0], &state->actions[1],
                    (size_t)(state->action_count - 1) * sizeof(state->actions[0]));
            state->action_count--;
            pthread_mutex_unlock(&state->action_mutex);
            (void)msgsnd(state->ipc->command_queue_id, &message, MSG_PAYLOAD_SIZE, 0);
        } else {
            pthread_mutex_unlock(&state->action_mutex);
        }
    }
    return NULL;
}

void run_alerts_watcher(ipc_context_t *ipc, const runtime_config_t *config)
{
    watcher_state_t state = {.ipc = ipc, .config = config};
    pthread_t evaluator;
    pthread_t actions;
    sigset_t signal_set;
    int signal_number;
    pthread_mutex_init(&state.action_mutex, NULL);
    pthread_cond_init(&state.action_condition, NULL);
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
    if (pthread_create(&evaluator, NULL, evaluator_thread, &state) != 0 ||
        pthread_create(&actions, NULL, action_thread, &state) != 0) _exit(EXIT_FAILURE);
    sigwait(&signal_set, &signal_number);
    (void)signal_number;
    state.stopping = 1;
    pthread_mutex_lock(&state.action_mutex);
    pthread_cond_broadcast(&state.action_condition);
    pthread_mutex_unlock(&state.action_mutex);
    pthread_mutex_lock(&ipc->telemetry->mutex);
    pthread_cond_broadcast(&ipc->telemetry->cond_update);
    pthread_mutex_unlock(&ipc->telemetry->mutex);
    pthread_join(evaluator, NULL);
    pthread_join(actions, NULL);
    pthread_cond_destroy(&state.action_condition);
    pthread_mutex_destroy(&state.action_mutex);
}
