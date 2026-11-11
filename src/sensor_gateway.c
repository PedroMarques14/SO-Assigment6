#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"

#define GATEWAY_BUFFER_SIZE 4096

typedef struct {
    reading_t reading;
    int sentinel;
} buffer_item_t;

typedef struct {
    buffer_item_t *items;
    size_t size;
    size_t in;
    size_t out;
    pthread_mutex_t mutex;
    sem_t slots;
    sem_t pending;
    ipc_context_t *ipc;
    const runtime_config_t *config;
    int pipe_fd;
    volatile sig_atomic_t stopping;
    sem_t *log_sem;
} gateway_state_t;

static gateway_state_t *active_gateway;

static void request_gateway_stop(int signal_number)
{
    (void)signal_number;
    if (active_gateway != NULL) active_gateway->stopping = 1;
}

static int wait_semaphore(sem_t *semaphore)
{
    int result;
    do {
        result = sem_wait(semaphore);
    } while (result == -1 && errno == EINTR);
    return result;
}

static int simulated_time(const global_stats_shm_t *stats, int time_unit_ms)
{
    struct timespec now;
    long long elapsed_ms;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1 || time_unit_ms <= 0) return 0;
    elapsed_ms = ((long long)now.tv_sec - stats->start_time.tv_sec) * 1000LL;
    elapsed_ms += (now.tv_nsec - stats->start_time.tv_nsec) / 1000000LL;
    return elapsed_ms > 0 ? (int)(elapsed_ms / time_unit_ms) : 0;
}

static int enqueue(gateway_state_t *state, const buffer_item_t *item)
{
    if (wait_semaphore(&state->slots) == -1) return -1;
    if (pthread_mutex_lock(&state->mutex) != 0) return -1;
    state->items[state->in] = *item;
    state->in = (state->in + 1U) % state->size;
    pthread_mutex_unlock(&state->mutex);
    return sem_post(&state->pending);
}

static int dequeue(gateway_state_t *state, buffer_item_t *item)
{
    if (wait_semaphore(&state->pending) == -1) return -1;
    if (pthread_mutex_lock(&state->mutex) != 0) return -1;
    *item = state->items[state->out];
    state->out = (state->out + 1U) % state->size;
    pthread_mutex_unlock(&state->mutex);
    return sem_post(&state->slots);
}

static int parse_reading(char *line, reading_t *reading)
{
    char *separator;
    char *value_text;
    char *end;
    long value;

    separator = strchr(line, '#');
    if (separator == NULL || separator == line) return -1;
    *separator = '\0';
    value_text = strchr(separator + 1, '#');
    if (value_text == NULL || value_text == separator + 1 || value_text[1] == '\0') return -1;
    *value_text = '\0';
    value_text++;
    if (strchr(value_text, '#') != NULL) return -1;
    value = strtol(value_text, &end, 10);
    if (*end != '\0' || end == value_text || value < INT_MIN || value > INT_MAX) return -1;
    if (strlen(line) >= SENSOR_ID_SIZE || strlen(separator + 1) >= KEY_SIZE) return -1;
    (void)snprintf(reading->sensor_id, sizeof(reading->sensor_id), "%s", line);
    (void)snprintf(reading->key, sizeof(reading->key), "%s", separator + 1);
    reading->value = (int)value;
    return 0;
}

static void *reader_thread(void *argument)
{
    gateway_state_t *state = argument;
    char input[GATEWAY_BUFFER_SIZE];
    size_t used = 0;

    while (!state->stopping) {
        ssize_t count = read(state->pipe_fd, input + used, sizeof(input) - used - 1U);
        if (count == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (count == 0) continue;
        used += (size_t)count;
        input[used] = '\0';
        char *line_start = input;
        char *newline;
        while ((newline = memchr(line_start, '\n', (size_t)(input + used - line_start))) != NULL) {
            reading_t reading;
            *newline = '\0';
            if (strcmp(line_start, "SHUTDOWN") == 0) {
                state->stopping = 1;
                break;
            }
            if (parse_reading(line_start, &reading) == 0) {
                buffer_item_t item = {.reading = reading, .sentinel = 0};
                if (enqueue(state, &item) == -1) state->stopping = 1;
            }
            line_start = newline + 1;
        }
        if (line_start != input) {
            size_t remaining = (size_t)(input + used - line_start);
            memmove(input, line_start, remaining);
            used = remaining;
        }
        if (used == sizeof(input) - 1U) used = 0;
    }

    buffer_item_t sentinel = {.sentinel = 1};
    (void)enqueue(state, &sentinel);
    return NULL;
}

static void update_sensor(telemetry_shm_t *telemetry, const reading_t *reading, int timestamp)
{
    int sensor_index = -1;
    for (int index = 0; index < telemetry->total_sensors; index++) {
        if (strcmp(telemetry->sensors[index].sensor_id, reading->sensor_id) == 0) {
            sensor_index = index;
            break;
        }
    }
    if (sensor_index == -1 && telemetry->total_sensors < MAX_SENSORS) {
        sensor_index = telemetry->total_sensors++;
        (void)snprintf(telemetry->sensors[sensor_index].sensor_id, SENSOR_ID_SIZE, "%s",
                       reading->sensor_id);
    }
    if (sensor_index >= 0) {
        telemetry->sensors[sensor_index].online = 1;
        (void)snprintf(telemetry->sensors[sensor_index].last_key, KEY_SIZE, "%s", reading->key);
        telemetry->sensors[sensor_index].last_value = reading->value;
        telemetry->sensors[sensor_index].last_seen_ut = timestamp;
        telemetry->sensors[sensor_index].total_readings++;
    }
}

static void update_key(telemetry_shm_t *telemetry, const reading_t *reading)
{
    int key_index = -1;
    for (int index = 0; index < telemetry->total_keys; index++) {
        if (strcmp(telemetry->keys[index].key, reading->key) == 0) {
            key_index = index;
            break;
        }
    }
    if (key_index == -1 && telemetry->total_keys < MAX_KEYS) {
        key_index = telemetry->total_keys++;
        (void)snprintf(telemetry->keys[key_index].key, KEY_SIZE, "%s", reading->key);
        telemetry->keys[key_index].min_value = reading->value;
        telemetry->keys[key_index].max_value = reading->value;
    }
    if (key_index >= 0) {
        key_telemetry_t *key = &telemetry->keys[key_index];
        key->last_value = reading->value;
        if (reading->value < key->min_value) key->min_value = reading->value;
        if (reading->value > key->max_value) key->max_value = reading->value;
        key->sample_count++;
        key->avg_value += (reading->value - key->avg_value) / key->sample_count;
    }
}

static void append_reading_to_csv(const reading_t *reading, int timestamp, sem_t *log_sem)
{
    char path[256];
    (void)snprintf(path, sizeof(path), "history/%s.csv", reading->key);
    if (log_sem != NULL) {
        int r;
        do { r = sem_wait(log_sem); } while (r == -1 && errno == EINTR);
    }
    FILE *f = fopen(path, "a");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) fprintf(f, "timestamp_ut,sensor_id,valor\n");
        fprintf(f, "%d,%s,%d\n", timestamp, reading->sensor_id, reading->value);
        fclose(f);
    }
    if (log_sem != NULL) sem_post(log_sem);
}

static void *consumer_thread(void *argument)
{
    gateway_state_t *state = argument;
    buffer_item_t item;
    while (dequeue(state, &item) == 0) {
        if (item.sentinel) break;
        if (pthread_mutex_lock(&state->ipc->global_stats->mutex) != 0) break;
        if (pthread_mutex_lock(&state->ipc->telemetry->mutex) != 0) {
            pthread_mutex_unlock(&state->ipc->global_stats->mutex);
            break;
        }
        int timestamp = simulated_time(state->ipc->global_stats, state->config->time_unit_ms);
        update_sensor(state->ipc->telemetry, &item.reading, timestamp);
        update_key(state->ipc->telemetry, &item.reading);
        state->ipc->telemetry->update_seq++;
        state->ipc->global_stats->total_readings++;
        pthread_cond_broadcast(&state->ipc->telemetry->cond_update);
        pthread_mutex_unlock(&state->ipc->telemetry->mutex);
        pthread_mutex_unlock(&state->ipc->global_stats->mutex);
        append_reading_to_csv(&item.reading, timestamp, state->log_sem);
    }
    return NULL;
}

void run_sensor_gateway(ipc_context_t *ipc, const runtime_config_t *config)
{
    gateway_state_t state = {0};
    pthread_t reader;
    pthread_t consumer;
    struct sigaction action = {0};

    state.size = (size_t)config->reading_buffer_size;
    state.items = calloc(state.size, sizeof(*state.items));
    if (state.items == NULL || state.size == 0) _exit(EXIT_FAILURE);
    state.ipc = ipc;
    state.config = config;
    state.log_sem = ipc->log_semaphore;
    state.pipe_fd = open(SENSOR_PIPE, O_RDWR);
    if (state.pipe_fd == -1 || pthread_mutex_init(&state.mutex, NULL) != 0 ||
        sem_init(&state.slots, 0, state.size) == -1 || sem_init(&state.pending, 0, 0) == -1) {
        free(state.items);
        if (state.pipe_fd != -1) close(state.pipe_fd);
        _exit(EXIT_FAILURE);
    }
    action.sa_handler = request_gateway_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) == -1) _exit(EXIT_FAILURE);
    active_gateway = &state;
    if (pthread_create(&reader, NULL, reader_thread, &state) != 0 ||
        pthread_create(&consumer, NULL, consumer_thread, &state) != 0) {
        state.stopping = 1;
        close(state.pipe_fd);
        _exit(EXIT_FAILURE);
    }
    pthread_join(reader, NULL);
    pthread_join(consumer, NULL);
    sem_destroy(&state.slots);
    sem_destroy(&state.pending);
    pthread_mutex_destroy(&state.mutex);
    close(state.pipe_fd);
    free(state.items);
    active_gateway = NULL;
}
