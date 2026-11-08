// Неименованные семафоры, лежащие прямо в разделяемой памяти.
#define _POSIX_C_SOURCE 200809L
#include "shm.h"

int sync_create(void) {
    if (sem_init(&S->u_lock, 1, 1) < 0) return -1;
    if (sem_init(&S->u_cook, 1, 0) < 0) return -1;
    if (sem_init(&S->u_done, 1, 0) < 0) return -1;
    for (int i = 0; i < cfg.n; i++)
        if (sem_init(&S->u_wake[i], 1, 0) < 0) return -1;
    return 0;
}

// дочерние процессы получили Shared через fork, отдельно подключаться не нужно
int sync_attach(void) { return 0; }

void sync_destroy(int unlink_all) {
    if (!unlink_all || !S) return;
    for (int i = 0; i < cfg.n; i++) sem_destroy(&S->u_wake[i]);
    sem_destroy(&S->u_done);
    sem_destroy(&S->u_cook);
    sem_destroy(&S->u_lock);
}

void sync_lock(void) { sem_wait_retry(&S->u_lock); }
void sync_unlock(void) { sem_post(&S->u_lock); }

void sync_wait_waiter(int idx) {
    sync_unlock();
    sem_wait_stop(&S->u_wake[idx]);
    sync_lock();
}

void sync_wake_waiter(int idx) { sem_post(&S->u_wake[idx]); }

void sync_wait_cook(void) {
    sync_unlock();
    sem_wait_stop(&S->u_cook);
    sync_lock();
}

void sync_wake_cook(void) { sem_post(&S->u_cook); }

void sync_wake_all(void) {
    for (int i = 0; i < cfg.n; i++) sem_post(&S->u_wake[i]);
    sem_post(&S->u_cook);
}

void sync_done_post(void) { sem_post(&S->u_done); }

int sync_done_wait(int timeout_ms) {
    (void)timeout_ms;
    return 0;
}
