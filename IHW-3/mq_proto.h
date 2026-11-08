#ifndef MQ_PROTO_H
#define MQ_PROTO_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "common.h"

#define Q_POT     "/cannibals_pot"        // дикари и повар -> горшок
#define Q_COOK    "/cannibals_cook"       // горшок -> повар
#define Q_REP     "/cannibals_rep_%d"     // горшок -> дикарь (ответы)
#define SEM_OUT   "/cannibals_out"        // замок вывода на экран и в журнал
#define SEM_READY "/cannibals_ready"      // каждый процесс сообщает о готовности

enum { M_START = 1, M_TAKE, M_FILL, M_DONE, M_COOK, M_GRANT, M_STARVED, M_STOP };
enum { MS_DONE = 1, MS_STARVED, MS_STOPPED };   // итоговый статус дикаря в M_DONE

typedef struct {
    int type;
    int id;         // номер дикаря, начиная с 0
    long a, b;
    Config cfg;     // заполнено только в M_START
} Msg;

static sem_t *out_sem;

static inline void out_lock(void) {
    while (sem_wait(out_sem) == -1 && errno == EINTR)
        ;
}
static inline void out_unlock(void) { sem_post(out_sem); }

static inline mqd_t q_create(const char *name, long maxmsg, int flags) {
    struct mq_attr at;
    memset(&at, 0, sizeof at);
    at.mq_maxmsg = maxmsg;
    at.mq_msgsize = sizeof(Msg);
    mq_unlink(name);    // остатки прошлого запуска
    return mq_open(name, O_CREAT | O_EXCL | flags, 0600, &at);
}

// ждёт, пока хозяин создаст очередь
static inline mqd_t q_open(const char *name, int flags) {
    for (int t = 0; t < 200 && !g_sig; t++) {
        mqd_t q = mq_open(name, flags);
        if (q != (mqd_t)-1) return q;
        msleep_raw(50);
    }
    return (mqd_t)-1;
}

static inline sem_t *sem_open_wait(const char *name) {
    for (int t = 0; t < 200 && !g_sig; t++) {
        sem_t *s = sem_open(name, O_RDWR);
        if (s != SEM_FAILED) return s;
        msleep_raw(50);
    }
    return SEM_FAILED;
}

static inline int q_send(mqd_t q, int type, int id, long a, long b) {
    Msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.id = id;
    m.a = a;
    m.b = b;
    while (mq_send(q, (const char *)&m, sizeof m, 0) < 0) {
        if (errno == EINTR && !g_sig) continue;
        return -1;
    }
    return 0;
}

// 0 - сообщение получено, -1 - прервано сигналом или ошибка
static inline int q_recv(mqd_t q, Msg *m) {
    while (mq_receive(q, (char *)m, sizeof *m, NULL) < 0) {
        if (errno == EINTR && !g_sig) continue;
        return -1;
    }
    return 0;
}

#endif
