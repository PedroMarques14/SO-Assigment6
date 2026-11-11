#ifndef HOME_IOT_IPC_H
#define HOME_IOT_IPC_H

#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>

#include "config.h"

typedef struct {
    long mtype;
    int console_id;
    int command_code;
    char origin[MESSAGE_ORIGIN_SIZE];
    char target[MESSAGE_TARGET_SIZE];
    int parameter;
    int secondary_parameter;
    char payload[MESSAGE_PAYLOAD_SIZE];
    int more;
} iot_msg_t;

#define MSG_PAYLOAD_SIZE (sizeof(iot_msg_t) - sizeof(long))

typedef struct {
    char sensor_id[SENSOR_ID_SIZE];
    char key[KEY_SIZE];
    int value;
} reading_t;

typedef struct {
    pthread_mutex_t mutex;
    int total_readings;
    int total_commands;
    int total_alerts;
    int total_actuator_starts;
    struct timespec start_time;
    int simulation_time_units;
} global_stats_shm_t;

typedef struct {
    char id[ACTUATOR_ID_SIZE];
    int is_high_power;
    int status;
    int on_until_ut;
    int cooldown_until_ut;
    pthread_mutex_t mutex;
} actuator_shm_entry_t;

typedef struct {
    actuator_shm_entry_t actuators[MAX_ACTUATORS];
    int total_actuators;
    int active_high_power_count;
    pthread_mutex_t global_mutex;
} actuator_shm_t;

typedef struct {
    char key[KEY_SIZE];
    int last_value;
    int min_value;
    int max_value;
    double avg_value;
    int sample_count;
} key_telemetry_t;

typedef struct {
    char sensor_id[SENSOR_ID_SIZE];
    char last_key[KEY_SIZE];
    int last_value;
    int online;
    int last_seen_ut;
    int total_readings;
} sensor_entry_t;

typedef struct {
    key_telemetry_t keys[MAX_KEYS];
    int total_keys;
    sensor_entry_t sensors[MAX_SENSORS];
    int total_sensors;
    pthread_mutex_t mutex;
    pthread_cond_t cond_update;
    unsigned long update_seq;
} telemetry_shm_t;

typedef struct {
    char rule_id[RULE_ID_SIZE];
    char key[KEY_SIZE];
    int min_limit;
    int max_limit;
    int severity;
    char auto_actuator[ACTUATOR_ID_SIZE];
    int auto_action_on;
    int auto_action_duration;
    int is_active_alert;
} alert_rule_entry_t;

typedef struct {
    char alert_id[ALERT_ID_SIZE];
    char rule_id[RULE_ID_SIZE];
    int value;
    int timestamp_ut;
    int state;
} alert_entry_t;

typedef struct {
    alert_rule_entry_t rules[MAX_RULES];
    int total_rules;
    alert_entry_t alerts[MAX_ALERTS];
    int total_alerts;
    int next_alert_seq;
    pthread_mutex_t mutex;
} alerts_shm_t;

typedef struct {
    int command_queue_id;
    int alerts_queue_id;
    int responses_queue_id;
    int global_stats_id;
    int actuators_id;
    int telemetry_id;
    int alerts_id;
    global_stats_shm_t *global_stats;
    actuator_shm_t *actuators;
    telemetry_shm_t *telemetry;
    alerts_shm_t *alerts;
    sem_t *energy_semaphore;
    sem_t *log_semaphore;
    char energy_semaphore_name[64];
    char log_semaphore_name[64];
    int sensor_fifo_owned;
    int command_fifo_owned;
    int shared_sync_initialized;
} ipc_context_t;

int ipc_create(ipc_context_t *context, const runtime_config_t *config);
void ipc_cleanup(ipc_context_t *context);
int ipc_attach(ipc_context_t *context);
void ipc_detach(ipc_context_t *context);

#endif
