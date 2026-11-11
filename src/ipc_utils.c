#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"

static void reset_context(ipc_context_t *context)
{
    memset(context, 0, sizeof(*context));
    context->command_queue_id = -1;
    context->alerts_queue_id = -1;
    context->responses_queue_id = -1;
    context->global_stats_id = -1;
    context->actuators_id = -1;
    context->telemetry_id = -1;
    context->alerts_id = -1;
}

static int create_fifo(const char *path, int *owned)
{
    struct stat status;
    if (mkfifo(path, 0666) == 0) {
        *owned = 1;
        return 0;
    }
    if (errno != EEXIST || stat(path, &status) == -1 || !S_ISFIFO(status.st_mode)) return -1;
    *owned = 0;
    return 0;
}

static int create_queues(ipc_context_t *context)
{
    key_t command_key = ftok(CONFIG_FILE, 'C');
    key_t alerts_key = ftok(CONFIG_FILE, 'A');
    key_t responses_key = ftok(CONFIG_FILE, 'R');
    if (command_key == (key_t)-1 || alerts_key == (key_t)-1 || responses_key == (key_t)-1) return -1;
    context->command_queue_id = msgget(command_key, IPC_CREAT | IPC_EXCL | 0600);
    context->alerts_queue_id = msgget(alerts_key, IPC_CREAT | IPC_EXCL | 0600);
    context->responses_queue_id = msgget(responses_key, IPC_CREAT | IPC_EXCL | 0600);
    return context->command_queue_id != -1 && context->alerts_queue_id != -1 &&
                   context->responses_queue_id != -1
               ? 0
               : -1;
}

static int create_shared_memory(ipc_context_t *context)
{
    key_t keys[4];
    int *ids[] = {&context->global_stats_id, &context->actuators_id,
                  &context->telemetry_id, &context->alerts_id};
    size_t sizes[] = {sizeof(global_stats_shm_t), sizeof(actuator_shm_t),
                      sizeof(telemetry_shm_t), sizeof(alerts_shm_t)};
    void **addresses[] = {(void **)&context->global_stats, (void **)&context->actuators,
                          (void **)&context->telemetry, (void **)&context->alerts};
    const int identifiers[] = {'1', '2', '3', '4'};

    for (size_t index = 0; index < 4; index++) {
        keys[index] = ftok(CONFIG_FILE, identifiers[index]);
        if (keys[index] == (key_t)-1) return -1;
        *ids[index] = shmget(keys[index], sizes[index], IPC_CREAT | IPC_EXCL | 0600);
        if (*ids[index] == -1) return -1;
        *addresses[index] = shmat(*ids[index], NULL, 0);
        if (*addresses[index] == (void *)-1) {
            *addresses[index] = NULL;
            return -1;
        }
        memset(*addresses[index], 0, sizes[index]);
    }
    return 0;
}

static int initialize_shared_synchronization(ipc_context_t *context,
                                             const runtime_config_t *config)
{
    pthread_mutexattr_t mutex_attribute;
    pthread_condattr_t condition_attribute;
    if (pthread_mutexattr_init(&mutex_attribute) != 0 ||
        pthread_mutexattr_setpshared(&mutex_attribute, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_condattr_init(&condition_attribute) != 0 ||
        pthread_condattr_setpshared(&condition_attribute, PTHREAD_PROCESS_SHARED) != 0) return -1;

    int result = pthread_mutex_init(&context->global_stats->mutex, &mutex_attribute);
    result |= pthread_mutex_init(&context->actuators->global_mutex, &mutex_attribute);
    for (int index = 0; result == 0 && index < MAX_ACTUATORS; index++) {
        result |= pthread_mutex_init(&context->actuators->actuators[index].mutex, &mutex_attribute);
    }
    result |= pthread_mutex_init(&context->telemetry->mutex, &mutex_attribute);
    result |= pthread_mutex_init(&context->alerts->mutex, &mutex_attribute);
    result |= pthread_cond_init(&context->telemetry->cond_update, &condition_attribute);
    pthread_condattr_destroy(&condition_attribute);
    pthread_mutexattr_destroy(&mutex_attribute);
    if (result != 0) return -1;

    context->shared_sync_initialized = 1;
    if (clock_gettime(CLOCK_MONOTONIC, &context->global_stats->start_time) == -1) return -1;
    context->actuators->total_actuators = config->actuator_count;
    for (int index = 0; index < config->actuator_count; index++) {
        (void)snprintf(context->actuators->actuators[index].id, ACTUATOR_ID_SIZE, "%s",
                       config->actuator_ids[index]);
        context->actuators->actuators[index].is_high_power = config->actuator_high_power[index];
    }
    context->alerts->next_alert_seq = 1;
    return 0;
}

static int open_named_semaphores(ipc_context_t *context, const runtime_config_t *config)
{
    int user_id = (int)getuid();
    if (snprintf(context->energy_semaphore_name, sizeof(context->energy_semaphore_name),
                 "%s%d", SEM_ENERGY_PREFIX, user_id) < 0 ||
        snprintf(context->log_semaphore_name, sizeof(context->log_semaphore_name),
                 "%s%d", SEM_LOG_PREFIX, user_id) < 0) return -1;
    context->energy_semaphore = sem_open(context->energy_semaphore_name, O_CREAT | O_EXCL,
                                         0644, (unsigned int)config->max_energy_budget);
    if (context->energy_semaphore == SEM_FAILED) {
        context->energy_semaphore = NULL;
        return -1;
    }
    context->log_semaphore = sem_open(context->log_semaphore_name, O_CREAT | O_EXCL, 0644, 1);
    if (context->log_semaphore == SEM_FAILED) {
        context->log_semaphore = NULL;
        return -1;
    }
    return 0;
}

static void cleanup_orphan_ipc(void)
{
    key_t key;
    int id;
    int user_id = (int)getuid();
    char sem_name[64];

    /* Queues */
    const int queue_ids[] = {'C', 'A', 'R'};
    for (int i = 0; i < 3; i++) {
        key = ftok(CONFIG_FILE, queue_ids[i]);
        if (key != (key_t)-1) {
            id = msgget(key, 0600);
            if (id != -1) msgctl(id, IPC_RMID, NULL);
        }
    }
    /* SHM */
    const int shm_ids[] = {'1', '2', '3', '4'};
    size_t shm_sizes[] = {sizeof(global_stats_shm_t), sizeof(actuator_shm_t),
                          sizeof(telemetry_shm_t), sizeof(alerts_shm_t)};
    for (int i = 0; i < 4; i++) {
        key = ftok(CONFIG_FILE, shm_ids[i]);
        if (key != (key_t)-1) {
            id = shmget(key, shm_sizes[i], 0600);
            if (id != -1) shmctl(id, IPC_RMID, NULL);
        }
    }
    /* Semaphores */
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_ENERGY_PREFIX, user_id);
    sem_unlink(sem_name);
    snprintf(sem_name, sizeof(sem_name), "%s%d", SEM_LOG_PREFIX, user_id);
    sem_unlink(sem_name);
}

int ipc_create(ipc_context_t *context, const runtime_config_t *config)
{
    if (context == NULL || config == NULL) return -1;
    reset_context(context);
    cleanup_orphan_ipc();
    if (create_queues(context) == -1 || create_shared_memory(context) == -1 ||
        initialize_shared_synchronization(context, config) == -1 ||
        open_named_semaphores(context, config) == -1 ||
        create_fifo(SENSOR_PIPE, &context->sensor_fifo_owned) == -1 ||
        create_fifo(COMMAND_PIPE, &context->command_fifo_owned) == -1) {
        ipc_cleanup(context);
        return -1;
    }
    return 0;
}

int ipc_attach(ipc_context_t *context)
{
    if (context == NULL) return -1;
    reset_context(context);

    /* 1. Attach to existing message queues */
    key_t command_key = ftok(CONFIG_FILE, 'C');
    key_t alerts_key  = ftok(CONFIG_FILE, 'A');
    key_t responses_key = ftok(CONFIG_FILE, 'R');
    if (command_key == (key_t)-1 || alerts_key == (key_t)-1 || responses_key == (key_t)-1) {
        ipc_detach(context);
        return -1;
    }
    context->command_queue_id   = msgget(command_key,   0600);
    context->alerts_queue_id    = msgget(alerts_key,    0600);
    context->responses_queue_id = msgget(responses_key, 0600);
    if (context->command_queue_id == -1 || context->alerts_queue_id == -1 ||
        context->responses_queue_id == -1) {
        ipc_detach(context);
        return -1;
    }

    /* 2. Attach to existing SHM segments */
    const int shm_chars[] = {'1', '2', '3', '4'};
    size_t shm_sizes[] = {sizeof(global_stats_shm_t), sizeof(actuator_shm_t),
                          sizeof(telemetry_shm_t), sizeof(alerts_shm_t)};
    void **shm_ptrs[] = {(void **)&context->global_stats, (void **)&context->actuators,
                         (void **)&context->telemetry,    (void **)&context->alerts};
    for (int i = 0; i < 4; i++) {
        key_t key = ftok(CONFIG_FILE, shm_chars[i]);
        if (key == (key_t)-1) { ipc_detach(context); return -1; }
        int id = shmget(key, shm_sizes[i], 0600);
        if (id == -1) { ipc_detach(context); return -1; }
        *shm_ptrs[i] = shmat(id, NULL, 0);
        if (*shm_ptrs[i] == (void *)-1) {
            *shm_ptrs[i] = NULL;
            ipc_detach(context);
            return -1;
        }
    }

    /* 3. Open existing named semaphores */
    int user_id = (int)getuid();
    if (snprintf(context->energy_semaphore_name, sizeof(context->energy_semaphore_name),
                 "%s%d", SEM_ENERGY_PREFIX, user_id) < 0 ||
        snprintf(context->log_semaphore_name, sizeof(context->log_semaphore_name),
                 "%s%d", SEM_LOG_PREFIX, user_id) < 0) {
        ipc_detach(context);
        return -1;
    }
    context->energy_semaphore = sem_open(context->energy_semaphore_name, 0);
    if (context->energy_semaphore == SEM_FAILED) {
        context->energy_semaphore = NULL;
        ipc_detach(context);
        return -1;
    }
    context->log_semaphore = sem_open(context->log_semaphore_name, 0);
    if (context->log_semaphore == SEM_FAILED) {
        context->log_semaphore = NULL;
        ipc_detach(context);
        return -1;
    }
    return 0;
}

void ipc_detach(ipc_context_t *context)
{
    if (context == NULL) return;
    if (context->energy_semaphore != NULL) sem_close(context->energy_semaphore);
    if (context->log_semaphore != NULL)    sem_close(context->log_semaphore);
    /* Do NOT sem_unlink — the owner (home_iot) will do that */
    if (context->global_stats != NULL) shmdt(context->global_stats);
    if (context->actuators    != NULL) shmdt(context->actuators);
    if (context->telemetry    != NULL) shmdt(context->telemetry);
    if (context->alerts       != NULL) shmdt(context->alerts);
    /* Do NOT shmctl IPC_RMID, do NOT msgctl IPC_RMID, do NOT unlink FIFOs */
    reset_context(context);
}

void ipc_cleanup(ipc_context_t *context)
{
    if (context == NULL) return;
    if (context->energy_semaphore != NULL) sem_close(context->energy_semaphore);
    if (context->log_semaphore != NULL) sem_close(context->log_semaphore);
    if (context->energy_semaphore_name[0] != '\0') sem_unlink(context->energy_semaphore_name);
    if (context->log_semaphore_name[0] != '\0') sem_unlink(context->log_semaphore_name);
    if (context->command_queue_id != -1) msgctl(context->command_queue_id, IPC_RMID, NULL);
    if (context->alerts_queue_id != -1) msgctl(context->alerts_queue_id, IPC_RMID, NULL);
    if (context->responses_queue_id != -1) msgctl(context->responses_queue_id, IPC_RMID, NULL);
    if (context->shared_sync_initialized) {
        pthread_mutex_destroy(&context->global_stats->mutex);
        pthread_mutex_destroy(&context->actuators->global_mutex);
        for (int index = 0; index < MAX_ACTUATORS; index++) {
            pthread_mutex_destroy(&context->actuators->actuators[index].mutex);
        }
        pthread_mutex_destroy(&context->telemetry->mutex);
        pthread_cond_destroy(&context->telemetry->cond_update);
        pthread_mutex_destroy(&context->alerts->mutex);
    }
    if (context->global_stats != NULL) shmdt(context->global_stats);
    if (context->actuators != NULL) shmdt(context->actuators);
    if (context->telemetry != NULL) shmdt(context->telemetry);
    if (context->alerts != NULL) shmdt(context->alerts);
    if (context->global_stats_id != -1) shmctl(context->global_stats_id, IPC_RMID, NULL);
    if (context->actuators_id != -1) shmctl(context->actuators_id, IPC_RMID, NULL);
    if (context->telemetry_id != -1) shmctl(context->telemetry_id, IPC_RMID, NULL);
    if (context->alerts_id != -1) shmctl(context->alerts_id, IPC_RMID, NULL);
    if (context->sensor_fifo_owned) unlink(SENSOR_PIPE);
    if (context->command_fifo_owned) unlink(COMMAND_PIPE);
    reset_context(context);
}
