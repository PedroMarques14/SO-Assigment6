#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "home_iot.h"
#include "ipc.h"
#include "log.h"

int load_config(const char *path, runtime_config_t *config);
void run_sensor_gateway(ipc_context_t *ipc, const runtime_config_t *config);
void run_actuator_controller(ipc_context_t *ipc, const runtime_config_t *config);
void run_alerts_watcher(ipc_context_t *ipc, const runtime_config_t *config);
void run_historian(ipc_context_t *ipc, const runtime_config_t *config);
void *run_command_dispatcher(void *argument);

typedef struct {
    ipc_context_t *ipc;
    const runtime_config_t *config;
    int pipe_fd;
} dispatcher_state_t;

static _Atomic int shutdown_requested;
static _Atomic int stats_requested;
static _Atomic int snapshot_requested;
static _Atomic int child_changed;

static void request_shutdown(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM)
        atomic_store_explicit(&shutdown_requested, 1, memory_order_relaxed);
    else if (signal_number == SIGUSR1)
        atomic_store_explicit(&stats_requested, 1, memory_order_relaxed);
    else if (signal_number == SIGUSR2)
        atomic_store_explicit(&snapshot_requested, 1, memory_order_relaxed);
    else if (signal_number == SIGCHLD)
        atomic_store_explicit(&child_changed, 1, memory_order_relaxed);
}

static int install_signal_handlers(void)
{
    struct sigaction action = {0};
    action.sa_handler = request_shutdown;
    if (sigemptyset(&action.sa_mask) == -1) {
        return -1;
    }
    if (sigaction(SIGINT, &action, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGUSR1, &action, NULL) == -1 || sigaction(SIGUSR2, &action, NULL) == -1 ||
        sigaction(SIGCHLD, &action, NULL) == -1) return -1;
    return 0;
}

static pid_t start_component(void (*entry_point)(ipc_context_t *, const runtime_config_t *),
                             ipc_context_t *ipc, const runtime_config_t *config)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction ignore = {0};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGINT, &ignore, NULL);
        sigaction(SIGUSR1, &ignore, NULL);
        sigaction(SIGUSR2, &ignore, NULL);
        sigaction(SIGCHLD, &ignore, NULL);
        entry_point(ipc, config);
        _exit(EXIT_SUCCESS);
    }
    return pid;
}

static void send_fifo_sentinel(const char *path, const char *message)
{
    int file_descriptor = open(path, O_RDWR);
    if (file_descriptor == -1) return;
    size_t message_length = strlen(message);
    if (write(file_descriptor, message, message_length) != (ssize_t)message_length) {
        perror("write FIFO sentinel");
    }
    close(file_descriptor);
}

static void print_summary(const ipc_context_t *ipc)
{
    pthread_mutex_lock(&ipc->global_stats->mutex);
    pthread_mutex_lock(&ipc->telemetry->mutex);
    printf("Home IoT: readings=%d commands=%d alerts=%d\n",
           ipc->global_stats->total_readings, ipc->global_stats->total_commands,
           ipc->global_stats->total_alerts);
    for (int index = 0; index < ipc->telemetry->total_keys; index++) {
        key_telemetry_t *key = &ipc->telemetry->keys[index];
        printf("%s min=%d max=%d avg=%.1f count=%d\n", key->key, key->min_value,
               key->max_value, key->avg_value, key->sample_count);
    }
    pthread_mutex_unlock(&ipc->telemetry->mutex);
    pthread_mutex_unlock(&ipc->global_stats->mutex);
    fflush(stdout);
}

int main(void)
{
    runtime_config_t config;
    ipc_context_t ipc;
    pid_t children[4];
    size_t child_count = 0;
    pthread_t dispatcher_thread;
    dispatcher_state_t dispatcher;
    int command_fd;
    pid_t historian_pid = -1;

    if (install_signal_handlers() == -1 || load_config(CONFIG_FILE, &config) == -1 ||
        ipc_create(&ipc, &config) == -1) {
        perror("home_iot initialization");
        return EXIT_FAILURE;
    }

    log_set_semaphore(ipc.log_semaphore);
    if (log_event(LOG_INFO, "SYSTEM", "START", "Home IoT manager started") == -1) {
        perror("log_event");
    }

    command_fd = open(COMMAND_PIPE, O_RDWR);
    if (command_fd == -1) {
        perror("open COMMAND_PIPE");
        ipc_cleanup(&ipc);
        return EXIT_FAILURE;
    }
    dispatcher.ipc = &ipc;
    dispatcher.config = &config;
    dispatcher.pipe_fd = command_fd;
    sigset_t fullmask, oldmask;
    sigfillset(&fullmask);
    pthread_sigmask(SIG_BLOCK, &fullmask, &oldmask);
    if (pthread_create(&dispatcher_thread, NULL, run_command_dispatcher, &dispatcher) != 0) {
        perror("pthread_create dispatcher");
        close(command_fd);
        ipc_cleanup(&ipc);
        return EXIT_FAILURE;
    }
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    void (*components[])(ipc_context_t *, const runtime_config_t *) = {
        run_sensor_gateway,
        run_actuator_controller,
        run_alerts_watcher,
        run_historian
    };

    for (size_t index = 0; index < ARRAY_SIZE(components); index++) {
        children[child_count] = start_component(components[index], &ipc, &config);
        if (children[child_count] == -1) {
            perror("fork");
            atomic_store_explicit(&shutdown_requested, 1, memory_order_relaxed);
            break;
        }
        child_count++;
    }
    if (child_count == ARRAY_SIZE(components)) historian_pid = children[3];

    sigset_t waitmask;
    sigemptyset(&waitmask);
    while (!atomic_load_explicit(&shutdown_requested, memory_order_relaxed)) {
        sigsuspend(&waitmask);
        if (atomic_exchange_explicit(&stats_requested, 0, memory_order_relaxed)) print_summary(&ipc);
        if (atomic_exchange_explicit(&snapshot_requested, 0, memory_order_relaxed) && historian_pid > 0)
            kill(historian_pid, SIGUSR2);
        if (atomic_exchange_explicit(&child_changed, 0, memory_order_relaxed)) {
            int status;
            pid_t ended;
            while ((ended = waitpid(-1, &status, WNOHANG)) > 0) {
                if (!atomic_load_explicit(&shutdown_requested, memory_order_relaxed)) {
                    log_event(LOG_ERROR, "SYSTEM", "CHILD_EXIT", "A child exited unexpectedly");
                    atomic_store_explicit(&shutdown_requested, 1, memory_order_relaxed);
                }
            }
        }
    }

    send_fifo_sentinel(SENSOR_PIPE, "SHUTDOWN\n");
    send_fifo_sentinel(COMMAND_PIPE, "0#shutdown#\n");
    pthread_join(dispatcher_thread, NULL);
    close(command_fd);
    for (size_t index = 0; index < child_count; index++) {
        if (kill(children[index], SIGTERM) == -1 && errno != ESRCH) {
            perror("kill");
        }
    }
    for (size_t index = 0; index < child_count; index++) {
        while (waitpid(children[index], NULL, 0) == -1 && errno == EINTR) {
        }
    }

    if (log_event(LOG_INFO, "SYSTEM", "STOP", "Home IoT manager stopped") == -1) {
        perror("log_event");
    }
    ipc_cleanup(&ipc);
    return EXIT_SUCCESS;
}
