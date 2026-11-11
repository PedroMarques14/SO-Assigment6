#include <errno.h>
#include <semaphore.h>

#include "sync.h"

int wait_sem(sem_t *semaphore)
{
    int result;
    do {
        result = sem_wait(semaphore);
    } while (result == -1 && errno == EINTR);
    return result;
}
