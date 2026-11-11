#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"

static volatile sig_atomic_t exit_requested;
static volatile sig_atomic_t alert_thread_stop;

typedef struct {
    int alert_queue;
    pthread_mutex_t *screen_mutex;
} alert_thread_state_t;

static void request_exit(int signal_number)
{
    (void)signal_number;
    exit_requested = 1;
}

static void interrupt_alert_thread(int signal_number)
{
    (void)signal_number;
    alert_thread_stop = 1;
}

static void *alert_thread(void *argument)
{
    alert_thread_state_t *state = argument;
    struct sigaction action = {0};
    sigset_t blocked;
    iot_msg_t message;
    action.sa_handler = interrupt_alert_thread;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    while (!alert_thread_stop) {
        ssize_t received = msgrcv(state->alert_queue, &message, MSG_PAYLOAD_SIZE, 0, 0);
        if (received == -1) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_mutex_lock(state->screen_mutex);
        printf("\n[ALERT] %s", message.payload);
        fflush(stdout);
        pthread_mutex_unlock(state->screen_mutex);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int console_id;
    int command_fd;
    int response_queue;
    int alert_queue;
    pthread_t alerts_thread;
    pthread_mutex_t screen_mutex = PTHREAD_MUTEX_INITIALIZER;
    alert_thread_state_t alert_state;
    char line[256];
    key_t response_key;
    struct sigaction exit_action = {0};
    if (argc != 2 || (console_id = atoi(argv[1])) <= 0) {
        fprintf(stderr, "Usage: %s <console_id>\n", argv[0]);
        return EXIT_FAILURE;
    }
    exit_action.sa_handler = request_exit;
    sigemptyset(&exit_action.sa_mask);
    if (sigaction(SIGINT, &exit_action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    response_key = ftok(CONFIG_FILE, 'R');
    key_t alert_key = ftok(CONFIG_FILE, 'A');
    if (response_key == (key_t)-1 || alert_key == (key_t)-1 ||
        (response_queue = msgget(response_key, 0600)) == -1 ||
        (alert_queue = msgget(alert_key, 0600)) == -1) {
        perror("MQ_RESPONSES");
        return EXIT_FAILURE;
    }
    command_fd = open(COMMAND_PIPE, O_WRONLY);
    if (command_fd == -1) {
        perror("COMMAND_PIPE");
        return EXIT_FAILURE;
    }
    alert_state.alert_queue = alert_queue;
    alert_state.screen_mutex = &screen_mutex;
    iot_msg_t stale_alert;
    while (msgrcv(alert_queue, &stale_alert, MSG_PAYLOAD_SIZE, 0, IPC_NOWAIT) != -1) {
    }
    if (pthread_create(&alerts_thread, NULL, alert_thread, &alert_state) != 0) {
        perror("pthread_create alerts");
        close(command_fd);
        return EXIT_FAILURE;
    }

    while (!exit_requested) {
        pthread_mutex_lock(&screen_mutex);
        printf("HomeIoT [%d]> ", console_id);
        fflush(stdout);
        pthread_mutex_unlock(&screen_mutex);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (errno == EINTR) clearerr(stdin);
            break;
        }
        char *command = line;
        char *argument;
        char message[512];
        size_t length;
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "exit") == 0) break;
        while (*command == ' ') command++;
        argument = strchr(command, ' ');
        if (argument != NULL) {
            *argument++ = '\0';
            while (*argument == ' ') argument++;
        } else {
            argument = "";
        }
        if (strcmp(command, "stats") == 0 && argument[0] == '\0') argument = "ALL";
        length = (size_t)snprintf(message, sizeof(message), "%d#%s#%s\n", console_id,
                                  command, argument);
        if (length >= sizeof(message) || write(command_fd, message, length) != (ssize_t)length) {
            perror("COMMAND_PIPE");
            break;
        }
        iot_msg_t response;
        ssize_t received;
        do {
            received = msgrcv(response_queue, &response, MSG_PAYLOAD_SIZE, console_id, 0);
        } while (received == -1 && errno == EINTR);
        if (received == -1) {
            perror("MQ_RESPONSES");
            break;
        }
        pthread_mutex_lock(&screen_mutex);
        fputs(response.payload, stdout);
        pthread_mutex_unlock(&screen_mutex);
        while (response.more) {
            do {
                received = msgrcv(response_queue, &response, MSG_PAYLOAD_SIZE, console_id, 0);
            } while (received == -1 && errno == EINTR);
            if (received == -1) break;
            pthread_mutex_lock(&screen_mutex);
            fputs(response.payload, stdout);
            pthread_mutex_unlock(&screen_mutex);
        }
    }
    exit_requested = 1;
    pthread_kill(alerts_thread, SIGUSR1);
    pthread_join(alerts_thread, NULL);
    close(command_fd);
    return EXIT_SUCCESS;
}
