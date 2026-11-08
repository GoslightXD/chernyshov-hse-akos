#ifndef SHM_H
#define SHM_H

#include <errno.h>
#include <semaphore.h>
#include <stdatomic.h>
#include "common.h"

#define SHM_NAME "/cannibals_shm"

enum { ST_NONE, ST_RUNNING, ST_DONE, ST_STARVED, ST_STOPPED };

typedef struct {
    Config cfg;
    long long start_ns;
    atomic_int ready, stop, finished;

    // состояние модели, под замком
    int pot;
    long supply_left;
    int request_pending, cooking, cook_started;
    long empties, requests, filled, taken, fills, cook_busy;
    int wq[MAX_SAVAGES], wq_head, wq_len;   // очередь ожидающих повара
    struct { int st, meals; long wait_ms; } sv[MAX_SAVAGES];

    // неименованные семафоры (используются только в sync_unnamed.c)
    sem_t u_lock, u_cook, u_done, u_wake[MAX_SAVAGES];
} Shared;

extern Shared *S;

int  stopping(void);

int  shm_create_new(void);
int  shm_attach_existing(void);
void shm_cleanup(void);

int  savage_run(int idx);
int  cook_run(void);

void model_stop(void);
void model_finish(void);
void print_state(void);
void print_stats(void);

// блокировка и пробуждение процессов; реализации в sync_named.c и sync_unnamed.c
int  sync_create(void);
int  sync_attach(void);
void sync_destroy(int unlink_all);
void sync_lock(void);
void sync_unlock(void);
void sync_wait_waiter(int idx);     // вызывать под замком, вернётся тоже под ним
void sync_wake_waiter(int idx);
void sync_wait_cook(void);
void sync_wake_cook(void);
void sync_wake_all(void);
void sync_done_post(void);
int  sync_done_wait(int timeout_ms);    // 1 - получен сигнал, 0 - таймаут

// ждёт семафор, но выходит раньше при остановке
static inline void sem_wait_stop(sem_t *s) {
    while (sem_wait(s) == -1) {
        if (errno != EINTR || stopping()) return;
    }
}

static inline void sem_wait_retry(sem_t *s) {
    while (sem_wait(s) == -1 && errno == EINTR)
        ;
}

#endif
