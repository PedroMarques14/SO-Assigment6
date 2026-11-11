#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

#include "config.h"
#include "home_iot.h"
#include "ipc.h"

#define COMMAND_BUFFER_SIZE 512

typedef struct {
    ipc_context_t *ipc;
    const runtime_config_t *config;
    int pipe_fd;
} dispatcher_state_t;

static int simulation_time(const dispatcher_state_t *state)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    long long elapsed = (long long)(now.tv_sec - state->ipc->global_stats->start_time.tv_sec) * 1000LL;
    elapsed += (now.tv_nsec - state->ipc->global_stats->start_time.tv_nsec) / 1000000LL;
    return state->config->time_unit_ms > 0 ? (int)(elapsed / state->config->time_unit_ms) : 0;
}

static int send_response(const ipc_context_t *ipc, int console_id, const char *text)
{
    iot_msg_t message = {0};
    size_t offset = 0;
    size_t length;
    if (console_id <= 0) return 0;
    length = strlen(text);
    do {
        size_t chunk = length - offset;
        if (chunk >= sizeof(message.payload)) chunk = sizeof(message.payload) - 1U;
        memset(&message, 0, sizeof(message));
        message.mtype = console_id;
        message.console_id = console_id;
        message.more = offset + chunk < length;
        memcpy(message.payload, text + offset, chunk);
        message.payload[chunk] = '\0';
        if (msgsnd(ipc->responses_queue_id, &message, MSG_PAYLOAD_SIZE, 0) == -1) return -1;
        offset += chunk;
    } while (offset < length);
    return 0;
}

static void dispatch_stats(dispatcher_state_t *state, int console_id, const char *argument)
{
    char response[MESSAGE_PAYLOAD_SIZE];
    int written = 0;
    const char *requested_key = argument[0] == '\0' ? "ALL" : argument;
    if (pthread_mutex_lock(&state->ipc->global_stats->mutex) != 0) return;
    if (pthread_mutex_lock(&state->ipc->telemetry->mutex) != 0) {
        pthread_mutex_unlock(&state->ipc->global_stats->mutex);
        return;
    }
    for (int index = 0; index < state->ipc->telemetry->total_keys; index++) {
        key_telemetry_t *key = &state->ipc->telemetry->keys[index];
        if (strcmp(requested_key, "ALL") != 0 && strcmp(requested_key, key->key) != 0) continue;
        written += snprintf(response + written, sizeof(response) - (size_t)written,
                            "%s min=%d max=%d avg=%.1f count=%d\n", key->key,
                            key->min_value, key->max_value, key->avg_value, key->sample_count);
        if (written >= (int)sizeof(response)) break;
    }
    if (written == 0) snprintf(response, sizeof(response), "No statistics for %s\n", requested_key);
    pthread_mutex_unlock(&state->ipc->telemetry->mutex);
    pthread_mutex_unlock(&state->ipc->global_stats->mutex);
    (void)send_response(state->ipc, console_id, response);
}

static void dispatch_sensors(dispatcher_state_t *state, int console_id)
{
    char response[MESSAGE_PAYLOAD_SIZE] = {0};
    int written = 0;
    int current_time = simulation_time(state);
    if (pthread_mutex_lock(&state->ipc->telemetry->mutex) != 0) return;
    for (int index = 0; index < state->ipc->telemetry->total_sensors; index++) {
        sensor_entry_t *sensor = &state->ipc->telemetry->sensors[index];
        sensor->online = current_time - sensor->last_seen_ut <= state->config->sensor_timeout;
        written += snprintf(response + written, sizeof(response) - (size_t)written,
                            "%s %s readings=%d last=%d\n", sensor->sensor_id,
                            sensor->online ? "ONLINE" : "OFFLINE", sensor->total_readings,
                            sensor->last_seen_ut);
        if (written >= (int)sizeof(response)) break;
    }
    if (written == 0) snprintf(response, sizeof(response), "No sensors\n");
    pthread_mutex_unlock(&state->ipc->telemetry->mutex);
    (void)send_response(state->ipc, console_id, response);
}

static void dispatch_reset(dispatcher_state_t *state, int console_id)
{
    if (pthread_mutex_lock(&state->ipc->global_stats->mutex) != 0) return;
    if (pthread_mutex_lock(&state->ipc->telemetry->mutex) != 0) {
        pthread_mutex_unlock(&state->ipc->global_stats->mutex);
        return;
    }
    state->ipc->global_stats->total_readings = 0;
    state->ipc->telemetry->total_keys = 0;
    for (int index = 0; index < state->ipc->telemetry->total_sensors; index++) {
        state->ipc->telemetry->sensors[index].total_readings = 0;
        state->ipc->telemetry->sensors[index].last_seen_ut = 0;
        state->ipc->telemetry->sensors[index].online = 0;
    }
    pthread_mutex_unlock(&state->ipc->telemetry->mutex);
    pthread_mutex_unlock(&state->ipc->global_stats->mutex);
    send_response(state->ipc, console_id, "OK metrics reset\n");
}

static void dispatch_list_rules(dispatcher_state_t *state, int console_id)
{
    char response[MESSAGE_PAYLOAD_SIZE] = {0};
    int written = 0;
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    for (int index = 0; index < state->ipc->alerts->total_rules; index++) {
        alert_rule_entry_t *rule = &state->ipc->alerts->rules[index];
        written += snprintf(response + written, sizeof(response) - (size_t)written,
                            "%s key=%s min=%d max=%d severity=%d\n", rule->rule_id,
                            rule->key, rule->min_limit, rule->max_limit, rule->severity);
        if (written >= (int)sizeof(response)) break;
    }
    if (written == 0) snprintf(response, sizeof(response), "No rules\n");
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    send_response(state->ipc, console_id, response);
}

static void dispatch_remove_rule(dispatcher_state_t *state, int console_id, const char *argument)
{
    int found = 0;
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    for (int index = 0; index < state->ipc->alerts->total_rules; index++) {
        if (strcmp(state->ipc->alerts->rules[index].rule_id, argument) == 0) {
            memmove(&state->ipc->alerts->rules[index], &state->ipc->alerts->rules[index + 1],
                    (size_t)(state->ipc->alerts->total_rules - index - 1) * sizeof(alert_rule_entry_t));
            state->ipc->alerts->total_rules--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    send_response(state->ipc, console_id, found ? "OK rule removed\n" : "ERROR rule not found\n");
}

static void dispatch_status(dispatcher_state_t *state, int console_id)
{
    char response[MESSAGE_PAYLOAD_SIZE] = {0};
    int written = 0;
    if (pthread_mutex_lock(&state->ipc->actuators->global_mutex) != 0) return;
    written += snprintf(response, sizeof(response), "energy=%d/%d\n",
                        state->ipc->actuators->active_high_power_count,
                        state->config->max_energy_budget);
    for (int index = 0; index < state->ipc->actuators->total_actuators; index++) {
        actuator_shm_entry_t *actuator = &state->ipc->actuators->actuators[index];
        written += snprintf(response + written, sizeof(response) - (size_t)written,
                            "%s status=%d\n", actuator->id, actuator->status);
        if (written >= (int)sizeof(response)) break;
    }
    pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
    (void)send_response(state->ipc, console_id, response);
}

static void dispatch_add_rule(dispatcher_state_t *state, int console_id, const char *argument)
{
    alert_rule_entry_t rule = {0};
    char action[ACTUATOR_ID_SIZE];
    char action_state[8];
    int action_on;
    int action_duration;
    int parsed = sscanf(argument, "%15s key: %31s min: %d max: %d severity: %d",
                        rule.rule_id, rule.key, &rule.min_limit, &rule.max_limit, &rule.severity);
    if (parsed != 5 || rule.min_limit > rule.max_limit || rule.severity < 1 || rule.severity > 3) {
        send_response(state->ipc, console_id, "ERROR invalid rule syntax\n");
        return;
    }
    char *action_text = strstr(argument, "action:");
    if (action_text != NULL) {
        action_text += 7;
        while (*action_text == ' ') action_text++;
    }
    if (action_text != NULL && sscanf(action_text, "%19[^:]:%7[^:]:%d", action, action_state,
                                      &action_duration) == 3 &&
        (strcmp(action_state, "ON") == 0 || strcmp(action_state, "OFF") == 0)) {
        action_on = strcmp(action_state, "ON") == 0;
        (void)snprintf(rule.auto_actuator, sizeof(rule.auto_actuator), "%s", action);
        rule.auto_action_on = action_on;
        rule.auto_action_duration = action_duration;
    }
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    if (state->ipc->alerts->total_rules >= state->config->max_rules) {
        pthread_mutex_unlock(&state->ipc->alerts->mutex);
        send_response(state->ipc, console_id, "ERROR rule limit reached\n");
        return;
    }
    state->ipc->alerts->rules[state->ipc->alerts->total_rules++] = rule;
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    send_response(state->ipc, console_id, "OK rule added\n");
}

static void dispatch_alerts(dispatcher_state_t *state, int console_id)
{
    char response[MESSAGE_PAYLOAD_SIZE] = {0};
    int written = 0;
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    for (int index = 0; index < state->ipc->alerts->total_alerts; index++) {
        alert_entry_t *alert = &state->ipc->alerts->alerts[index];
        written += snprintf(response + written, sizeof(response) - (size_t)written,
                            "%s rule=%s value=%d state=%d\n", alert->alert_id,
                            alert->rule_id, alert->value, alert->state);
        if (written >= (int)sizeof(response)) break;
    }
    if (written == 0) snprintf(response, sizeof(response), "No alerts\n");
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    send_response(state->ipc, console_id, response);
}

static void dispatch_ack(dispatcher_state_t *state, int console_id, const char *argument)
{
    int found = 0;
    if (pthread_mutex_lock(&state->ipc->alerts->mutex) != 0) return;
    for (int index = 0; index < state->ipc->alerts->total_alerts; index++) {
        if (strcmp(state->ipc->alerts->alerts[index].alert_id, argument) == 0) {
            state->ipc->alerts->alerts[index].state = ALERT_ACKED;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&state->ipc->alerts->mutex);
    send_response(state->ipc, console_id, found ? "OK alert acknowledged\n" : "ERROR alert not found\n");
}

static void dispatch_actuator(dispatcher_state_t *state, int console_id, const char *argument)
{
    char copy[MESSAGE_PAYLOAD_SIZE];
    char actuator[ACTUATOR_ID_SIZE];
    char action[8];
    char *duration_text;
    iot_msg_t message = {0};
    int duration = -1;

    (void)snprintf(copy, sizeof(copy), "%s", argument);
    if (sscanf(copy, "%19s %7s", actuator, action) != 2 ||
        (strcmp(action, "ON") != 0 && strcmp(action, "OFF") != 0)) {
        char response[MESSAGE_PAYLOAD_SIZE];
        (void)snprintf(response, sizeof(response), "ERROR invalid actuator syntax\n");
        (void)send_response(state->ipc, console_id, response);
        return;
    }
    duration_text = strstr(copy, "duration:");
    if (duration_text != NULL && sscanf(duration_text + 9, "%d", &duration) != 1) duration = -1;
    message.mtype = 2;
    message.console_id = console_id;
    (void)snprintf(message.target, sizeof(message.target), "%s", actuator);
    (void)snprintf(message.payload, sizeof(message.payload), "%s", action);
    message.parameter = duration;
    if (msgsnd(state->ipc->command_queue_id, &message, MSG_PAYLOAD_SIZE, 0) == -1) {
        char response[MESSAGE_PAYLOAD_SIZE];
        (void)snprintf(response, sizeof(response), "ERROR actuator queue: %s\n", strerror(errno));
        (void)send_response(state->ipc, console_id, response);
    }
}

static void dispatch_schedule(dispatcher_state_t *state, int console_id, const char *argument)
{
    iot_msg_t message = {0};
    char schedule_id[RULE_ID_SIZE];
    char actuator[ACTUATOR_ID_SIZE];
    int at;
    int duration;
    int now = simulation_time(state);
    if (sscanf(argument, "%15s at: %d actuator: %19s duration: %d", schedule_id, &at,
               actuator, &duration) != 4 || at <= now || duration <= 0) {
        send_response(state->ipc, console_id, "ERROR invalid schedule instant or syntax\n");
        return;
    }
    message.mtype = 2;
    message.console_id = console_id;
    (void)snprintf(message.origin, sizeof(message.origin), "%s", schedule_id);
    (void)snprintf(message.target, sizeof(message.target), "%s", actuator);
    (void)snprintf(message.payload, sizeof(message.payload), "SCHEDULE");
    message.parameter = at;
    message.secondary_parameter = duration;
    if (msgsnd(state->ipc->command_queue_id, &message, MSG_PAYLOAD_SIZE, 0) == -1) {
        send_response(state->ipc, console_id, "ERROR schedule queue\n");
    } else {
        send_response(state->ipc, console_id, "OK schedule accepted\n");
    }
}

static void dispatch_command(dispatcher_state_t *state, int console_id, const char *command,
                             const char *argument)
{
    if (strcmp(command, "stats") == 0) dispatch_stats(state, console_id, argument);
    else if (strcmp(command, "sensors") == 0) dispatch_sensors(state, console_id);
    else if (strcmp(command, "status") == 0) dispatch_status(state, console_id);
    else if (strcmp(command, "actuator") == 0) dispatch_actuator(state, console_id, argument);
    else if (strcmp(command, "schedule") == 0) dispatch_schedule(state, console_id, argument);
    else if (strcmp(command, "add_rule") == 0) dispatch_add_rule(state, console_id, argument);
    else if (strcmp(command, "alerts") == 0) dispatch_alerts(state, console_id);
    else if (strcmp(command, "ack") == 0) dispatch_ack(state, console_id, argument);
    else if (strcmp(command, "reset") == 0) dispatch_reset(state, console_id);
    else if (strcmp(command, "list_rules") == 0) dispatch_list_rules(state, console_id);
    else if (strcmp(command, "remove_rule") == 0) dispatch_remove_rule(state, console_id, argument);
    else {
        char response[MESSAGE_PAYLOAD_SIZE];
        (void)snprintf(response, sizeof(response), "ERROR unsupported command: %s\n", command);
        (void)send_response(state->ipc, console_id, response);
    }
}

void *run_command_dispatcher(void *argument)
{
    dispatcher_state_t *state = argument;
    char buffer[COMMAND_BUFFER_SIZE];
    size_t used = 0;
    while (1) {
        ssize_t count = read(state->pipe_fd, buffer + used, sizeof(buffer) - used - 1U);
        if (count == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (count == 0) continue;
        used += (size_t)count;
        buffer[used] = '\0';
        char *line = buffer;
        char *newline;
        while ((newline = strchr(line, '\n')) != NULL) {
            int console_id;
            char *command;
            char *argument;
            *newline = '\0';
            command = strchr(line, '#');
            if (command == NULL) {
                line = newline + 1;
                continue;
            }
            *command++ = '\0';
            argument = strchr(command, '#');
            if (argument == NULL) {
                line = newline + 1;
                continue;
            }
            *argument++ = '\0';
            console_id = atoi(line);
            if (strcmp(command, "shutdown") == 0) return NULL;
            if (console_id > 0) {
                if (pthread_mutex_lock(&state->ipc->global_stats->mutex) == 0) {
                    state->ipc->global_stats->total_commands++;
                    pthread_mutex_unlock(&state->ipc->global_stats->mutex);
                }
            }
            dispatch_command(state, console_id, command, argument);
            line = newline + 1;
        }
        if (line != buffer) {
            size_t remaining = (size_t)(buffer + used - line);
            memmove(buffer, line, remaining);
            used = remaining;
        }
        if (used == sizeof(buffer) - 1U) used = 0;
    }
    return NULL;
}
