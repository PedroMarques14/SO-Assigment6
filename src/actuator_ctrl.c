#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>

#include "home_iot.h"
#include "ipc.h"

typedef struct {
    ipc_context_t *ipc;
    const runtime_config_t *config;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int active_timers;
    pthread_mutex_t schedule_mutex;
    pthread_cond_t schedule_condition;
    iot_msg_t schedules[MAX_RULES];
    int schedule_count;
} actuator_state_t;

typedef struct {
    actuator_state_t *state;
    int actuator_index;
    int duration_ut;
} timer_argument_t;

static int controller_stopping;
static pthread_mutex_t controller_stop_mutex = PTHREAD_MUTEX_INITIALIZER;

static int is_controller_stopping(void)
{
    int stopping;
    pthread_mutex_lock(&controller_stop_mutex);
    stopping = controller_stopping;
    pthread_mutex_unlock(&controller_stop_mutex);
    return stopping;
}

static void process_message(actuator_state_t *state, const iot_msg_t *message);

static void wake_request_thread(int signal_number)
{
    (void)signal_number;
}

static void sleep_units(int units, int milliseconds)
{
    struct timespec delay = {
        .tv_sec = (time_t)((long long)units * milliseconds / 1000),
        .tv_nsec = (long)(((long long)units * milliseconds % 1000) * 1000000LL)
    };
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
    }
}

static int find_actuator(const actuator_shm_t *actuators, const char *target)
{
    for (int index = 0; index < actuators->total_actuators; index++) {
        if (strcmp(actuators->actuators[index].id, target) == 0) return index;
    }
    return -1;
}

static void send_result(const ipc_context_t *ipc, int console_id, const char *text)
{
    iot_msg_t response = {0};
    if (console_id <= 0) return;
    response.mtype = console_id;
    response.console_id = console_id;
    response.more = 0;
    (void)snprintf(response.payload, sizeof(response.payload), "%s", text);
    (void)msgsnd(ipc->responses_queue_id, &response, MSG_PAYLOAD_SIZE, 0);
}

static void timer_finished(actuator_state_t *state)
{
    pthread_mutex_lock(&state->mutex);
    state->active_timers--;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

static void *timer_thread(void *argument)
{
    timer_argument_t *timer = argument;
    actuator_state_t *state = timer->state;
    actuator_shm_entry_t *actuator = &state->ipc->actuators->actuators[timer->actuator_index];
    sleep_units(timer->duration_ut, state->config->time_unit_ms);
    if (pthread_mutex_lock(&state->ipc->actuators->global_mutex) == 0) {
        if (pthread_mutex_lock(&actuator->mutex) == 0) {
            if (actuator->status == ACTUATOR_ON) {
                actuator->status = ACTUATOR_COOLDOWN;
                actuator->cooldown_until_ut = timer->duration_ut +
                    state->config->actuator_cooldown_min;
                if (actuator->is_high_power) {
                    state->ipc->actuators->active_high_power_count--;
                    (void)sem_post(state->ipc->energy_semaphore);
                }
            }
            pthread_mutex_unlock(&actuator->mutex);
        }
        pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
    }
    sleep_units(state->config->actuator_cooldown_min, state->config->time_unit_ms);
    if (pthread_mutex_lock(&state->ipc->actuators->global_mutex) == 0) {
        if (pthread_mutex_lock(&actuator->mutex) == 0) {
            if (actuator->status == ACTUATOR_COOLDOWN) actuator->status = ACTUATOR_OFF;
            pthread_mutex_unlock(&actuator->mutex);
        }
        pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
    }
    timer_finished(state);
    free(timer);
    return NULL;
}

static int start_timer(actuator_state_t *state, int index, int duration)
{
    pthread_t thread;
    timer_argument_t *argument = calloc(1, sizeof(*argument));
    if (argument == NULL) return -1;
    argument->state = state;
    argument->actuator_index = index;
    argument->duration_ut = duration;
    pthread_mutex_lock(&state->mutex);
    state->active_timers++;
    pthread_mutex_unlock(&state->mutex);
    if (pthread_create(&thread, NULL, timer_thread, argument) != 0) {
        timer_finished(state);
        free(argument);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

static int current_simulation_time(const actuator_state_t *state)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed = (long long)(now.tv_sec - state->ipc->global_stats->start_time.tv_sec) * 1000LL;
    elapsed += (now.tv_nsec - state->ipc->global_stats->start_time.tv_nsec) / 1000000LL;
    return state->config->time_unit_ms > 0 ? (int)(elapsed / state->config->time_unit_ms) : 0;
}

static void *schedule_thread(void *argument)
{
    actuator_state_t *state = argument;
    while (!is_controller_stopping()) {
        pthread_mutex_lock(&state->schedule_mutex);
        while (state->schedule_count == 0 && !controller_stopping)
            pthread_cond_wait(&state->schedule_condition, &state->schedule_mutex);
        if (controller_stopping) {
            pthread_mutex_unlock(&state->schedule_mutex);
            break;
        }
        int selected = 0;
        for (int index = 1; index < state->schedule_count; index++)
            if (state->schedules[index].parameter < state->schedules[selected].parameter) selected = index;
        int wait_units = state->schedules[selected].parameter - current_simulation_time(state);
        if (wait_units > 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            long long milliseconds = (long long)wait_units * state->config->time_unit_ms;
            deadline.tv_sec += milliseconds / 1000;
            deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&state->schedule_condition, &state->schedule_mutex, &deadline);
            pthread_mutex_unlock(&state->schedule_mutex);
            continue;
        }
        iot_msg_t message = state->schedules[selected];
        memmove(&state->schedules[selected], &state->schedules[selected + 1],
                (size_t)(state->schedule_count - selected - 1) * sizeof(message));
        state->schedule_count--;
        pthread_mutex_unlock(&state->schedule_mutex);
        (void)snprintf(message.payload, sizeof(message.payload), "ON");
        message.parameter = message.secondary_parameter;
        message.console_id = 0;
        process_message(state, &message);
    }
    return NULL;
}

static void process_message(actuator_state_t *state, const iot_msg_t *message)
{
    if (strcmp(message->payload, "SCHEDULE") == 0) {
        pthread_mutex_lock(&state->schedule_mutex);
        if (state->schedule_count >= MAX_RULES) {
            pthread_mutex_unlock(&state->schedule_mutex);
            send_result(state->ipc, message->console_id, "ERROR schedule limit reached\n");
            return;
        }
        state->schedules[state->schedule_count++] = *message;
        pthread_cond_signal(&state->schedule_condition);
        pthread_mutex_unlock(&state->schedule_mutex);
        return;
    }
    int index;
    char response[MESSAGE_PAYLOAD_SIZE];
    int turning_on = strcmp(message->payload, "ON") == 0;
    if (!turning_on && strcmp(message->payload, "OFF") != 0) return;
    if (pthread_mutex_lock(&state->ipc->actuators->global_mutex) != 0) return;
    index = find_actuator(state->ipc->actuators, message->target);
    if (index < 0) {
        pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
        (void)snprintf(response, sizeof(response), "ERROR actuator not found: %s\n", message->target);
        send_result(state->ipc, message->console_id, response);
        return;
    }
    actuator_shm_entry_t *actuator = &state->ipc->actuators->actuators[index];
    pthread_mutex_lock(&actuator->mutex);
    if (turning_on) {
        if (actuator->status != ACTUATOR_OFF) {
            pthread_mutex_unlock(&actuator->mutex);
            pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
            send_result(state->ipc, message->console_id, "ERROR actuator busy\n");
            return;
        }
        if (actuator->is_high_power && sem_trywait(state->ipc->energy_semaphore) == -1) {
            pthread_mutex_unlock(&actuator->mutex);
            pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
            send_result(state->ipc, message->console_id, "ERROR energy budget exhausted\n");
            return;
        }
        actuator->status = ACTUATOR_ON;
        if (actuator->is_high_power) state->ipc->actuators->active_high_power_count++;
        pthread_mutex_lock(&state->ipc->global_stats->mutex);
        state->ipc->global_stats->total_actuator_starts++;
        pthread_mutex_unlock(&state->ipc->global_stats->mutex);
        pthread_mutex_unlock(&actuator->mutex);
        pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
        if (message->parameter > 0) (void)start_timer(state, index, message->parameter);
        (void)snprintf(response, sizeof(response), "OK %s ON\n", message->target);
        send_result(state->ipc, message->console_id, response);
        return;
    }
    if (actuator->status != ACTUATOR_ON) {
        pthread_mutex_unlock(&actuator->mutex);
        pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
        send_result(state->ipc, message->console_id, "ERROR actuator is not ON\n");
        return;
    }
    actuator->status = ACTUATOR_COOLDOWN;
    if (actuator->is_high_power) {
        state->ipc->actuators->active_high_power_count--;
        (void)sem_post(state->ipc->energy_semaphore);
    }
    pthread_mutex_unlock(&actuator->mutex);
    pthread_mutex_unlock(&state->ipc->actuators->global_mutex);
    (void)start_timer(state, index, 0);
    (void)snprintf(response, sizeof(response), "OK %s OFF\n", message->target);
    send_result(state->ipc, message->console_id, response);
}

static void *request_thread(void *argument)
{
    actuator_state_t *state = argument;
    iot_msg_t message;
    while (!is_controller_stopping()) {
        ssize_t received = msgrcv(state->ipc->command_queue_id, &message, MSG_PAYLOAD_SIZE, -2, 0);
        if (received == -1) {
            if (errno == EINTR) break;
            continue;
        }
        process_message(state, &message);
    }
    return NULL;
}

void run_actuator_controller(ipc_context_t *ipc, const runtime_config_t *config)
{
    actuator_state_t state = {.ipc = ipc, .config = config};
    pthread_t requests;
    pthread_t schedules;
    sigset_t blocked_signals;
    int received_signal;
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.condition, NULL);
    pthread_mutex_init(&state.schedule_mutex, NULL);
    pthread_cond_init(&state.schedule_condition, NULL);
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL);
    struct sigaction wake_action = {0};
    wake_action.sa_handler = wake_request_thread;
    sigemptyset(&wake_action.sa_mask);
    if (sigaction(SIGUSR1, &wake_action, NULL) == -1) _exit(EXIT_FAILURE);
    if (pthread_create(&requests, NULL, request_thread, &state) != 0 ||
        pthread_create(&schedules, NULL, schedule_thread, &state) != 0) _exit(EXIT_FAILURE);
    sigwait(&blocked_signals, &received_signal);
    (void)received_signal;
    pthread_mutex_lock(&controller_stop_mutex);
    controller_stopping = 1;
    pthread_mutex_unlock(&controller_stop_mutex);
    pthread_kill(requests, SIGUSR1);
    pthread_mutex_lock(&state.schedule_mutex);
    pthread_cond_broadcast(&state.schedule_condition);
    pthread_mutex_unlock(&state.schedule_mutex);
    pthread_join(requests, NULL);
    pthread_join(schedules, NULL);
    pthread_mutex_lock(&state.mutex);
    while (state.active_timers > 0) pthread_cond_wait(&state.condition, &state.mutex);
    pthread_mutex_unlock(&state.mutex);
    pthread_cond_destroy(&state.condition);
    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.schedule_condition);
    pthread_mutex_destroy(&state.schedule_mutex);
}
