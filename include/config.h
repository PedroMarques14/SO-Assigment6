#ifndef HOME_IOT_CONFIG_H
#define HOME_IOT_CONFIG_H

#define MAX_KEYS 50
#define MAX_SENSORS 20
#define MAX_ACTUATORS 10
#define MAX_RULES 30
#define MAX_ALERTS 100

#define SENSOR_PIPE "SENSOR_PIPE"
#define COMMAND_PIPE "COMMAND_PIPE"
#define CONFIG_FILE "config/config.txt"
#define LOG_FILE "logs/home_iot_log.txt"
#define SEM_ENERGY_PREFIX "/sem_home_energy_"
#define SEM_LOG_PREFIX "/sem_home_log_"

#define ACTUATOR_ID_SIZE 20
#define SENSOR_ID_SIZE 32
#define KEY_SIZE 32
#define RULE_ID_SIZE 16
#define ALERT_ID_SIZE 16
#define MESSAGE_ORIGIN_SIZE 20
#define MESSAGE_TARGET_SIZE 20
#define MESSAGE_PAYLOAD_SIZE 256

typedef struct {
	int time_unit_ms;
	int max_keys;
	int max_sensors;
	int max_actuators;
	int max_energy_budget;
	int max_rules;
	int max_alerts;
	int actuator_cooldown_min;
	int actuator_cooldown_max;
	int history_flush_interval;
	int snapshot_interval;
	int sensor_timeout;
	int reading_buffer_size;
	int actuator_count;
	char actuator_ids[MAX_ACTUATORS][ACTUATOR_ID_SIZE];
	int actuator_high_power[MAX_ACTUATORS];
} runtime_config_t;

#endif
