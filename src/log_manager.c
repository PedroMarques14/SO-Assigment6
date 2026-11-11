#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

static sem_t *log_semaphore;

void log_set_semaphore(sem_t *semaphore)
{
    log_semaphore = semaphore;
}

static const char *level_name(log_level_t level)
{
    switch (level) {
    case LOG_INFO:
        return "INFO";
    case LOG_WARNING:
        return "WARNING";
    case LOG_ERROR:
        return "ERROR";
    }
    return "UNKNOWN";
}

int log_event(log_level_t level, const char *component, const char *event, const char *details)
{
    if (log_semaphore != NULL) {
        while (sem_wait(log_semaphore) == -1 && errno == EINTR) {
        }
    }
    int file_descriptor = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file_descriptor == -1) {
        if (log_semaphore != NULL) sem_post(log_semaphore);
        return -1;
    }

    time_t now = time(NULL);
    struct tm timestamp;
    if (localtime_r(&now, &timestamp) == NULL) {
        close(file_descriptor);
        if (log_semaphore != NULL) sem_post(log_semaphore);
        return -1;
    }

    char line[512];
    int length = snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s] %s ",
                          timestamp.tm_year + 1900, timestamp.tm_mon + 1, timestamp.tm_mday,
                          timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec,
                          component, level_name(level), event);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        close(file_descriptor);
        if (log_semaphore != NULL) sem_post(log_semaphore);
        errno = EOVERFLOW;
        return -1;
    }

    size_t details_length = strlen(details);
    if ((size_t)length + details_length + 1 >= sizeof(line)) {
        close(file_descriptor);
        if (log_semaphore != NULL) sem_post(log_semaphore);
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(line + length, details, details_length);
    line[length + details_length] = '\n';
    length += (int)details_length + 1;

    ssize_t written = write(file_descriptor, line, (size_t)length);
    int saved_errno = errno;
    close(file_descriptor);
    if (log_semaphore != NULL) sem_post(log_semaphore);
    errno = saved_errno;
    return written == length ? 0 : -1;
}
