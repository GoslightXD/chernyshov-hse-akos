// Именованные POSIX семафоры: /dev/shm/sem.cannibals_*
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "shm.h"

#define N_LOCK "/cannibals_lock"
#define N_COOK "/cannibals_cook"
#define N_DONE "/cannibals_done"
#define N_WAKE "/cannibals_wake_%d"

static sem_t *lock_s, *cook_s, *done_s, *wake_s[MAX_SAVAGES];
static int count;

static sem_t *make(const char *name, unsigned value) {
    sem_unlink(name);   // остатки прошлого запуска
    return sem_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, value);
}

static sem_t *open_existing(const char *name) {
    for (int t = 0; t < 100; t++) {
        sem_t *s = sem_open(name, O_RDWR);
        if (s != SEM_FAILED) return s;
        msleep_raw(50);
    }
    return SEM_FAILED;
}

int sync_create(void) {
    count = cfg.n;
    lock_s = make(N_LOCK, 1);
    cook_s = make(N_COOK, 0);
    done_s = make(N_DONE, 0);
    if (lock_s == SEM_FAILED || cook_s == SEM_FAILED || done_s == SEM_FAILED) return -1;
    for (int i = 0; i < count; i++) {
        char name[48];
        snprintf(name, sizeof name, N_WAKE, i);
        wake_s[i] = make(name, 0);
        if (wake_s[i] == SEM_FAILED) return -1;
    }
    return 0;
}

int sync_attach(void) {
    count = cfg.n;
    lock_s = open_existing(N_LOCK);
    cook_s = open_existing(N_COOK);
    done_s = open_existing(N_DONE);
    if (lock_s == SEM_FAILED || cook_s == SEM_FAILED || done_s == SEM_FAILED) return -1;
    for (int i = 0; i < count; i++) {
        char name[48];
        snprintf(name, sizeof name, N_WAKE, i);
        wake_s[i] = open_existing(name);
        if (wake_s[i] == SEM_FAILED) return -1;
    }
    return 0;
}

static void close_one(sem_t *s) {
    if (s && s != SEM_FAILED) sem_close(s);
}

void sync_destroy(int unlink_all) {
    close_one(lock_s);
    close_one(cook_s);
    close_one(done_s);
    for (int i = 0; i < count; i++) close_one(wake_s[i]);
    if (!unlink_all) return;
    sem_unlink(N_LOCK);
    sem_unlink(N_COOK);
    sem_unlink(N_DONE);
    for (int i = 0; i < count; i++) {
        char name[48];
        snprintf(name, sizeof name, N_WAKE, i);
        sem_unlink(name);
    }
}

void sync_lock(void) { sem_wait_retry(lock_s); }
void sync_unlock(void) { sem_post(lock_s); }

void sync_wait_waiter(int idx) {
    sync_unlock();
    sem_wait_stop(wake_s[idx]);
    sync_lock();
}

void sync_wake_waiter(int idx) { sem_post(wake_s[idx]); }

void sync_wait_cook(void) {
    sync_unlock();
    sem_wait_stop(cook_s);
    sync_lock();
}

void sync_wake_cook(void) { sem_post(cook_s); }

void sync_wake_all(void) {
    for (int i = 0; i < count; i++) sem_post(wake_s[i]);
    sem_post(cook_s);
}

void sync_done_post(void) { sem_post(done_s); }

int sync_done_wait(int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return sem_timedwait(done_s, &ts) == 0;
}
