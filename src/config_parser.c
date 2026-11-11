#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static void set_defaults(runtime_config_t *config)
{
    config->time_unit_ms = 200;
    config->max_keys = MAX_KEYS;
    config->max_sensors = MAX_SENSORS;
    config->max_actuators = MAX_ACTUATORS;
    config->max_energy_budget = 2;
    config->max_rules = MAX_RULES;
    config->max_alerts = MAX_ALERTS;
    config->actuator_cooldown_min = 10;
    config->actuator_cooldown_max = 20;
    config->history_flush_interval = 50;
    config->snapshot_interval = 300;
    config->sensor_timeout = 50;
    config->reading_buffer_size = 64;
    config->actuator_count = 0;
}

static int parse_actuator(const char *key, const char *value, runtime_config_t *config)
{
    int number;
    char id[ACTUATOR_ID_SIZE];
    int high_power;
    if (sscanf(key, "ACTUATOR_%d", &number) != 1 || number < 1 || number > MAX_ACTUATORS ||
        sscanf(value, "%19[^:]:%d", id, &high_power) != 2 || (high_power != 0 && high_power != 1)) {
        return -1;
    }
    if (number > config->actuator_count) {
        config->actuator_count = number;
    }
    (void)snprintf(config->actuator_ids[number - 1], ACTUATOR_ID_SIZE, "%s", id);
    config->actuator_high_power[number - 1] = high_power;
    return 0;
}

static int parse_value(const char *key, const char *value, runtime_config_t *config)
{
    int *destination = NULL;
    if (strcmp(key, "TIME_UNIT_MS") == 0) destination = &config->time_unit_ms;
    else if (strcmp(key, "MAX_KEYS") == 0) destination = &config->max_keys;
    else if (strcmp(key, "MAX_SENSORS") == 0) destination = &config->max_sensors;
    else if (strcmp(key, "MAX_ACTUATORS") == 0) destination = &config->max_actuators;
    else if (strcmp(key, "MAX_ENERGY_BUDGET") == 0) destination = &config->max_energy_budget;
    else if (strcmp(key, "MAX_RULES") == 0) destination = &config->max_rules;
    else if (strcmp(key, "MAX_ALERTS") == 0) destination = &config->max_alerts;
    else if (strcmp(key, "ACTUATOR_COOLDOWN_MIN") == 0) destination = &config->actuator_cooldown_min;
    else if (strcmp(key, "ACTUATOR_COOLDOWN_MAX") == 0) destination = &config->actuator_cooldown_max;
    else if (strcmp(key, "HISTORY_FLUSH_INTERVAL") == 0) destination = &config->history_flush_interval;
    else if (strcmp(key, "SNAPSHOT_INTERVAL") == 0) destination = &config->snapshot_interval;
    else if (strcmp(key, "SENSOR_TIMEOUT") == 0) destination = &config->sensor_timeout;
    else if (strcmp(key, "READING_BUFFER_SIZE") == 0) destination = &config->reading_buffer_size;
    else if (strncmp(key, "ACTUATOR_", 9) == 0) return parse_actuator(key, value, config);
    else return 0;
    return sscanf(value, "%d", destination) == 1 ? 0 : -1;
}

static int validate_config(const runtime_config_t *config)
{
    return config->time_unit_ms > 0 && config->max_keys > 0 && config->max_keys <= MAX_KEYS &&
           config->max_sensors > 0 && config->max_sensors <= MAX_SENSORS &&
           config->max_actuators > 0 && config->max_actuators <= MAX_ACTUATORS &&
           config->max_energy_budget > 0 && config->max_rules > 0 && config->max_rules <= MAX_RULES &&
           config->max_alerts > 0 && config->max_alerts <= MAX_ALERTS &&
           config->actuator_cooldown_min >= 0 && config->actuator_cooldown_max >= config->actuator_cooldown_min &&
           config->reading_buffer_size > 0 && config->actuator_count <= config->max_actuators &&
           config->history_flush_interval > 0 && config->snapshot_interval > 0 &&
           config->sensor_timeout > 0 && config->time_unit_ms >= 10;
}

int load_config(const char *path, runtime_config_t *config)
{
    FILE *file = fopen(path, "r");
    char line[256];
    if (file == NULL || config == NULL) return -1;
    set_defaults(config);
    while (fgets(line, sizeof(line), file) != NULL) {
        char key[64];
        char value[192];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %191[^\n]", key, value) != 2) continue;
        if (parse_value(key, value, config) == -1) {
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return validate_config(config) ? 0 : -1;
}
