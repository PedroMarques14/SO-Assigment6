#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

static volatile sig_atomic_t stop_requested;

static void stop_sensor(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <sensor_id> <interval_ms> <key> <min> <max>\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sigaction action = {0};
    action.sa_handler = stop_sensor;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    int interval_ms = atoi(argv[2]);
    int minimum = atoi(argv[4]);
    int maximum = atoi(argv[5]);
    if (interval_ms < 1 || minimum > maximum || argv[1][0] == '\0' || argv[3][0] == '\0') {
        fprintf(stderr, "Invalid sensor arguments\n");
        return EXIT_FAILURE;
    }

    int file_descriptor = open(SENSOR_PIPE, O_WRONLY);
    if (file_descriptor == -1) {
        perror("open SENSOR_PIPE");
        return EXIT_FAILURE;
    }

    int value = minimum;
    while (!stop_requested) {
        char line[128];
        int length = snprintf(line, sizeof(line), "%s#%s#%d\n", argv[1], argv[3], value);
        if (length < 0 || (size_t)length >= sizeof(line) || write(file_descriptor, line, (size_t)length) == -1) {
            if (errno != EINTR) {
                break;
            }
        }
        value = value < maximum ? value + 1 : minimum;
        struct timespec delay = {
            .tv_sec = interval_ms / 1000,
            .tv_nsec = (long)(interval_ms % 1000) * 1000000L
        };
        nanosleep(&delay, NULL);
    }

    close(file_descriptor);
    return EXIT_SUCCESS;
}
