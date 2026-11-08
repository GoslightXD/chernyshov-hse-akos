#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shm.h"

Shared *S;
static int is_creator;

int stopping(void) { return g_sig || (S && atomic_load(&S->stop)); }

// ---------- разделяемая память ----------

int shm_create_new(void) {
    shm_unlink(SHM_NAME);   // остатки прошлого аварийного запуска
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        out("Ошибка shm_open: %s\n", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, sizeof(Shared)) < 0) {
        out("Ошибка ftruncate: %s\n", strerror(errno));
        close(fd);
        shm_unlink(SHM_NAME);
        return -1;
    }
    void *p = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        out("Ошибка mmap: %s\n", strerror(errno));
        shm_unlink(SHM_NAME);
        return -1;
    }
    S = p;
    is_creator = 1;
    S->cfg = cfg;
    S->start_ns = mono_ns();
    S->supply_left = cfg.supply;
    S->empties = 1;     // в начале горшок пуст
    if (sync_create() < 0) {
        shm_cleanup();
        return -1;
    }
    time_set_base(S->start_ns);
    set_stop_hook(stopping);
    atomic_store(&S->ready, 1);
    return 0;
}

int shm_attach_existing(void) {
    int fd = -1;
    for (int t = 0; t < 200 && !g_sig; t++) {
        fd = shm_open(SHM_NAME, O_RDWR, 0);
        if (fd >= 0) break;
        msleep_raw(50);
    }
    if (fd < 0) {
        out("Разделяемая память не найдена: сначала запустите программу-хозяина (cannibals_host)\n");
        return -1;
    }
    struct stat st;
    for (int t = 0; t < 200; t++) {
        if (fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(Shared)) break;
        msleep_raw(50);
    }
    void *p = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        out("Ошибка mmap: %s\n", strerror(errno));
        return -1;
    }
    S = p;
    for (int t = 0; t < 200 && !atomic_load(&S->ready); t++) msleep_raw(50);
    if (!atomic_load(&S->ready)) {
        out("Хозяин не завершил инициализацию\n");
        return -1;
    }
    cfg = S->cfg;
    time_set_base(S->start_ns);
    set_stop_hook(stopping);
    return sync_attach();
}

void shm_cleanup(void) {
    sync_destroy(is_creator);
    if (S) munmap(S, sizeof(Shared));
    S = NULL;
    if (is_creator) shm_unlink(SHM_NAME);
}

// ---------- модель (все функции ниже вызываются под замком) ----------

static void fatal(const char *msg) {
    out("НАРУШЕН ИНВАРИАНТ: %s\nАварийное завершение.\n", msg);
    _exit(2);
}

static void check(void) {
    if (S->pot < 0 || S->pot > cfg.cap) fatal("число порций вне [0, вместимость]");
    if (S->filled - S->taken != S->pot) fatal("баланс порций не сходится");
    if (S->requests > S->empties) fatal("запросов больше, чем опустошений");
}

static void enqueue(int idx) {
    S->wq[(S->wq_head + S->wq_len) % MAX_SAVAGES] = idx;
    S->wq_len++;
}

static void dequeue_self(int idx) {
    int k = 0;
    for (int i = 0; i < S->wq_len; i++) {
        int v = S->wq[(S->wq_head + i) % MAX_SAVAGES];
        if (v != idx) S->wq[(S->wq_head + k++) % MAX_SAVAGES] = v;
    }
    S->wq_len = k;
}

static int wake_first_waiter(void) {
    if (S->wq_len == 0) return 0;
    int idx = S->wq[S->wq_head];
    S->wq_head = (S->wq_head + 1) % MAX_SAVAGES;
    S->wq_len--;
    sync_wake_waiter(idx);
    return 1;
}

// 0 - дикарю больше есть не нужно
static int savage_eat(int idx) {
    int id = idx + 1;
    if (stopping()) {
        S->sv[idx].st = ST_STOPPED;
        return 0;
    }
    ev("Дикарь %d: хочет есть, пытается взять порцию (в горшке %d/%d)", id, S->pot, cfg.cap);
    for (;;) {
        if (S->pot > 0) {
            S->pot--;
            S->taken++;
            S->sv[idx].meals++;
            ev("Дикарь %d: получил порцию #%d, в горшке осталось %d", id, S->sv[idx].meals, S->pot);
            if (S->pot == 0) {
                S->empties++;
                wake_first_waiter();    // он обнаружит пустой горшок
            }
            check();
            if (cfg.meals > 0 && S->sv[idx].meals >= cfg.meals) {
                S->sv[idx].st = ST_DONE;
                ev("Дикарь %d: завершил питание (съел %d порц.)", id, S->sv[idx].meals);
                return 0;
            }
            return 1;
        }
        ev("Дикарь %d: обнаружил пустой горшок", id);
        if (cfg.supply > 0 && S->supply_left == 0) {
            S->sv[idx].st = ST_STARVED;
            ev("Дикарь %d: запас исчерпан, уходит голодным (съел %d порц.)", id, S->sv[idx].meals);
            wake_first_waiter();        // цепочка: следующий ожидающий тоже узнает
            return 0;
        }
        if (!S->request_pending) {
            S->request_pending = 1;
            S->requests++;
            ev("Дикарь %d: будит повара", id);
            sync_wake_cook();
        } else {
            ev("Дикарь %d: повар уже разбужен, ждёт без повторного запроса", id);
        }
        enqueue(idx);
        long t0 = now_ms();
        sync_wait_waiter(idx);
        S->sv[idx].wait_ms += now_ms() - t0;
        if (stopping()) {
            dequeue_self(idx);
            S->sv[idx].st = ST_STOPPED;
            return 0;
        }
    }
}

int savage_run(int idx) {
    unsigned rng = cfg.seed + 7919u * (unsigned)(idx + 1);
    sync_lock();
    if (S->sv[idx].st != ST_NONE) {
        sync_unlock();
        out("Дикарь %d уже запущен\n", idx + 1);
        return 1;
    }
    S->sv[idx].st = ST_RUNNING;
    sync_unlock();

    for (;;) {
        sleep_ms(rnd(&rng, cfg.tmin, cfg.tmax));
        sync_lock();
        int more = savage_eat(idx);
        sync_unlock();
        if (!more) break;
    }
    sync_done_post();
    return 0;
}

int cook_run(void) {
    unsigned rng = cfg.seed ^ 0x9e3779b9u;
    sync_lock();
    if (S->cook_started) {
        sync_unlock();
        out("Повар уже запущен\n");
        return 1;
    }
    S->cook_started = 1;
    for (;;) {
        sync_wait_cook();
        if (stopping() || atomic_load(&S->finished)) break;
        long start = now_ms();
        long dur = rnd(&rng, cfg.cmin, cfg.cmax);
        S->cooking = 1;
        ev("Повар: проснулся, начинает приготовление (~%ld мс)", dur);
        sync_unlock();
        sleep_ms(dur);
        sync_lock();
        if (stopping()) break;

        long took = now_ms() - start;
        ev("Повар: приготовление закончено (длилось %ld мс)", took);
        long portions = cfg.cap;
        if (cfg.supply > 0 && portions > S->supply_left) portions = S->supply_left;
        if (S->pot != 0) fatal("повар пытается наполнить непустой горшок");
        S->pot = (int)portions;
        S->filled += portions;
        if (cfg.supply > 0) S->supply_left -= portions;
        S->fills++;
        S->cook_busy += took;
        S->request_pending = 0;
        S->cooking = 0;
        if (cfg.supply > 0)
            ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d, остаток запаса %ld)",
               portions, S->pot, cfg.cap, S->supply_left);
        else
            ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d)", portions, S->pot, cfg.cap);
        ev("Повар: засыпает");
        // будим столько ожидающих, сколько порций; остальные ждут дальше
        for (long k = 0; k < portions; k++)
            if (!wake_first_waiter()) break;
        check();
    }
    sync_unlock();
    return 0;
}

// ---------- управление и отчёты (вызывает хозяин) ----------

void model_stop(void) {
    atomic_store(&S->stop, 1);
    sync_lock();
    sync_wake_all();
    sync_unlock();
}

void model_finish(void) {
    sync_lock();
    atomic_store(&S->finished, 1);
    sync_wake_cook();
    sync_unlock();
}

void print_state(void) {
    sync_lock();
    ev("Состояние: в горшке %d/%d, ждут повара %d, повар %s, запросов %ld",
       S->pot, cfg.cap, S->wq_len, S->cooking ? "готовит" : "спит", S->requests);
    sync_unlock();
}

// после остановки всех процессов, замок не нужен
void print_stats(void) {
    int done = 0, starved = 0, stopped = 0, none = 0;
    for (int i = 0; i < cfg.n; i++) {
        switch (S->sv[i].st) {
        case ST_DONE: done++; break;
        case ST_STARVED: starved++; break;
        case ST_NONE: none++; break;
        default: stopped++;
        }
    }
    out("\n===== Итог =====\n");
    if (atomic_load(&S->stop)) out("Причина завершения: прервано пользователем\n");
    else if (starved) out("Причина завершения: запас порций исчерпан, ожидающие завершены корректно\n");
    else out("Причина завершения: все дикари выполнили заданное число приёмов пищи\n");
    out("Время работы: %ld мс\n", now_ms());
    out("Повар: наполнений=%ld, время работы=%ld мс, запросов=%ld\n", S->fills, S->cook_busy, S->requests);
    out("Горшок: опустошений=%ld, положено порций=%ld, взято порций=%ld, осталось=%d\n",
        S->empties, S->filled, S->taken, S->pot);
    if (cfg.supply > 0) out("Запас: было %ld, осталось %ld\n", cfg.supply, S->supply_left);
    out("Дикари: наелись=%d, ушли голодными=%d, прерваны=%d, не запускались=%d\n", done, starved, stopped, none);
    out("  №   порций  ждал, мс   статус\n");
    for (int i = 0; i < cfg.n; i++) {
        int st = S->sv[i].st;
        const char *name = st == ST_DONE ? "наелся" : st == ST_STARVED ? "ушёл голодным"
                         : st == ST_NONE ? "не запускался" : "прерван";
        out(" %3d  %6d  %8ld   %s\n", i + 1, S->sv[i].meals, S->sv[i].wait_ms, name);
    }
}
