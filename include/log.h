#ifndef HOME_IOT_LOG_H
#define HOME_IOT_LOG_H

#include <semaphore.h>

typedef enum {
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
} log_level_t;

int log_event(log_level_t level, const char *component, const char *event, const char *details);
void log_set_semaphore(sem_t *semaphore);

#endif
