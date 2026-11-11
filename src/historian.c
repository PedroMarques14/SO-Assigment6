#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "home_iot.h"
#include "ipc.h"

typedef struct {
    ipc_context_t *ipc;
    const runtime_config_t *config;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    volatile sig_atomic_t stopping;
} historian_state_t;

static int current_time_units(const historian_state_t *state)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    long long elapsed = (long long)(now.tv_sec - state->ipc->global_stats->start_time.tv_sec) * 1000LL;
    elapsed += (now.tv_nsec - state->ipc->global_stats->start_time.tv_nsec) / 1000000LL;
    return state->config->time_unit_ms > 0 ? (int)(elapsed / state->config->time_unit_ms) : 0;
}

static void copy_telemetry(historian_state_t *state, telemetry_shm_t *copy)
{
    pthread_mutex_lock(&state->ipc->telemetry->mutex);
    memcpy(copy->keys, state->ipc->telemetry->keys, sizeof(copy->keys));
    copy->total_keys = state->ipc->telemetry->total_keys;
    memcpy(copy->sensors, state->ipc->telemetry->sensors, sizeof(copy->sensors));
    copy->total_sensors = state->ipc->telemetry->total_sensors;
    pthread_mutex_unlock(&state->ipc->telemetry->mutex);
}

static void write_snapshot(historian_state_t *state)
{
    telemetry_shm_t telemetry = {0};
    actuator_shm_t actuators = {0};
    alerts_shm_t alerts = {0};
    int timestamp = current_time_units(state);
    char path[128];
    copy_telemetry(state, &telemetry);
    pthread_mutex_lock(&state->ipc->actuators->global_mutex);
    memcpy(actuators.actuators, state->ipc->actuators->actuators, sizeof(actuators.actuators));
    actuators.total_actuators = state->ipc->actuators->total_actuators;
    actuators.active_high_power_count = state->ipc->actuators->active_high_power_count;
    pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
    pthread_mutex_lock(&state->ipc->alerts->mutex);
    memcpy(alerts.rules, state->ipc->alerts->rules, sizeof(alerts.rules));
    memcpy(alerts.alerts, state->ipc->alerts->alerts, sizeof(alerts.alerts));
    alerts.total_alerts = state->ipc->alerts->total_alerts;
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    (void)snprintf(path, sizeof(path), "results/stats_snapshot_%d.txt", timestamp);
    FILE *file = fopen(path, "w");
    if (file == NULL) return;
    fprintf(file, "timestamp_ut=%d\n", timestamp);
    fprintf(file, "[TELEMETRY]\n");
    for (int index = 0; index < telemetry.total_keys; index++) {
        key_telemetry_t *key = &telemetry.keys[index];
        fprintf(file, "%s min=%d max=%d avg=%.2f count=%d\n", key->key, key->min_value,
                key->max_value, key->avg_value, key->sample_count);
    }
    fprintf(file, "[ACTUATORS] energy=%d/%d\n", actuators.active_high_power_count,
            state->config->max_energy_budget);
    for (int index = 0; index < actuators.total_actuators; index++)
        fprintf(file, "%s status=%d\n", actuators.actuators[index].id,
                actuators.actuators[index].status);
    fprintf(file, "[ALERTS] total=%d\n", alerts.total_alerts);
    for (int index = 0; index < alerts.total_alerts; index++)
        fprintf(file, "%s rule=%s value=%d state=%d\n", alerts.alerts[index].alert_id,
                alerts.alerts[index].rule_id, alerts.alerts[index].value,
                alerts.alerts[index].state);
    fclose(file);
}

static void flush_history(historian_state_t *state)
{
    telemetry_shm_t telemetry = {0};
    int timestamp = current_time_units(state);
    copy_telemetry(state, &telemetry);
    for (int index = 0; index < telemetry.total_sensors; index++) {
        sensor_entry_t *sensor = &telemetry.sensors[index];
        if (sensor->last_key[0] == '\0') continue;
        char path[128];
        (void)snprintf(path, sizeof(path), "history/%s.csv", sensor->last_key);
        FILE *file = fopen(path, "a+");
        if (file == NULL) continue;
        fseek(file, 0, SEEK_END);
        if (ftell(file) == 0) fprintf(file, "timestamp_ut,sensor_id,valor\n");
        fprintf(file, "%d,%s,%d\n", timestamp, sensor->sensor_id, sensor->last_value);
        fclose(file);
    }
}

static void write_final_report(historian_state_t *state)
{
    telemetry_shm_t telemetry = {0};
    global_stats_shm_t stats = {0};
    copy_telemetry(state, &telemetry);
    pthread_mutex_lock(&state->ipc->global_stats->mutex);
    stats.total_readings = state->ipc->global_stats->total_readings;
    stats.total_commands = state->ipc->global_stats->total_commands;
    stats.total_alerts = state->ipc->global_stats->total_alerts;
    pthread_mutex_unlock(&state->ipc->global_stats->mutex);
    FILE *file = fopen("results/final_report.txt", "w");
    if (file == NULL) return;
    fprintf(file, "Home IoT final report\n");
    fprintf(file, "total_readings=%d\n", stats.total_readings);
    fprintf(file, "total_commands=%d\n", stats.total_commands);
    fprintf(file, "total_alerts=%d\n", stats.total_alerts);
    fprintf(file, "keys=%d sensors=%d\n", telemetry.total_keys, telemetry.total_sensors);
    fclose(file);
}

static void *flush_thread(void *argument)
{
    historian_state_t *state = argument;
    while (!state->stopping) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        long long milliseconds = (long long)state->config->history_flush_interval *
                                 state->config->time_unit_ms;
        deadline.tv_sec += milliseconds / 1000;
        deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&state->mutex);
        if (!state->stopping) pthread_cond_timedwait(&state->condition, &state->mutex, &deadline);
        pthread_mutex_unlock(&state->mutex);
        if (!state->stopping) flush_history(state);
    }
    return NULL;
}

static void *snapshot_thread(void *argument)
{
    historian_state_t *state = argument;
    while (!state->stopping) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        long long milliseconds = (long long)state->config->snapshot_interval *
                                 state->config->time_unit_ms;
        deadline.tv_sec += milliseconds / 1000;
        deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&state->mutex);
        if (!state->stopping) pthread_cond_timedwait(&state->condition, &state->mutex, &deadline);
        pthread_mutex_unlock(&state->mutex);
        if (!state->stopping) write_snapshot(state);
    }
    return NULL;
}

void run_historian(ipc_context_t *ipc, const runtime_config_t *config)
{
    historian_state_t state = {.ipc = ipc, .config = config};
    pthread_t flusher;
    pthread_t snapshots;
    sigset_t signals;
    int signal_number;
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.condition, NULL);
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    if (pthread_create(&flusher, NULL, flush_thread, &state) != 0 ||
        pthread_create(&snapshots, NULL, snapshot_thread, &state) != 0) _exit(EXIT_FAILURE);
    while (!state.stopping) {
        sigwait(&signals, &signal_number);
        if (signal_number == SIGUSR2) write_snapshot(&state);
        else if (signal_number == SIGTERM) {
            pthread_mutex_lock(&state.mutex);
            state.stopping = 1;
            pthread_cond_broadcast(&state.condition);
            pthread_mutex_unlock(&state.mutex);
        }
    }
    pthread_join(flusher, NULL);
    pthread_join(snapshots, NULL);
    flush_history(&state);
    write_snapshot(&state);
    write_final_report(&state);
    pthread_cond_destroy(&state.condition);
    pthread_mutex_destroy(&state.mutex);
}
