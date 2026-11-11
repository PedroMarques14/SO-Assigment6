#ifndef HOME_IOT_H
#define HOME_IOT_H

#include <signal.h>
#include <time.h>

#include "config.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    ACTUATOR_OFF = 0,
    ACTUATOR_ON = 1,
    ACTUATOR_COOLDOWN = 2
} actuator_status_t;

typedef enum {
    ALERT_ACTIVE = 0,
    ALERT_ACKED = 1,
    ALERT_RESOLVED = 2
} alert_state_t;

int simulated_time_units(const struct timespec *start_time, long time_unit_ms);

#endif
