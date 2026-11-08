// Горшок: владеет состоянием, принимает запросы по очереди сообщений,
// создаёт очереди и семафоры, печатает итог. Повар и дикари - отдельные программы.
#include <stdlib.h>
#include <unistd.h>
#include "mq_proto.h"

static mqd_t pot_q = (mqd_t)-1, cook_q = (mqd_t)-1, rep_q[MAX_SAVAGES];
static sem_t *ready_s;

static struct {
    int pot, pending, head, len, stopping;
    long supply_left, empties, requests, filled, taken, fills, cook_busy, cook_t0;
    int wq[MAX_SAVAGES];
    int meals[MAX_SAVAGES], st[MAX_SAVAGES];
    long wait_ms[MAX_SAVAGES], wait_t0[MAX_SAVAGES];
} w;

static void fatal(const char *msg) {
    out("НАРУШЕН ИНВАРИАНТ: %s\nАварийное завершение.\n", msg);
    _exit(2);
}

static void check(void) {
    if (w.pot < 0 || w.pot > cfg.cap) fatal("число порций вне [0, вместимость]");
    if (w.filled - w.taken != w.pot) fatal("баланс порций не сходится");
    if (w.requests > w.empties) fatal("запросов больше, чем опустошений");
}

static void reply(int id, int type) { q_send(rep_q[id], type, id, 0, 0); }

static void enqueue(int id) {
    w.wq[(w.head + w.len) % MAX_SAVAGES] = id;
    w.len++;
    w.wait_t0[id] = now_ms();
}

static void attempt(int id);

static int wake_first_waiter(void) {
    if (w.len == 0) return 0;
    int id = w.wq[w.head];
    w.head = (w.head + 1) % MAX_SAVAGES;
    w.len--;
    w.wait_ms[id] += now_ms() - w.wait_t0[id];
    attempt(id);    // разбуженный повторяет попытку
    return 1;
}

// попытка дикаря взять порцию; ответ уходит дикарю по его очереди
static void attempt(int id) {
    if (w.pot > 0) {
        w.pot--;
        w.taken++;
        w.meals[id]++;
        ev("Дикарь %d: получил порцию #%d, в горшке осталось %d", id + 1, w.meals[id], w.pot);
        reply(id, M_GRANT);
        if (w.pot == 0) {
            w.empties++;
            wake_first_waiter();    // он обнаружит пустой горшок
        }
        return;
    }
    ev("Дикарь %d: обнаружил пустой горшок", id + 1);
    if (cfg.supply > 0 && w.supply_left == 0) {
        ev("Дикарь %d: запас исчерпан, уходит голодным (съел %d порц.)", id + 1, w.meals[id]);
        reply(id, M_STARVED);
        wake_first_waiter();        // цепочка: следующий ожидающий тоже узнает
        return;
    }
    if (!w.pending) {
        w.pending = 1;
        w.requests++;
        w.cook_t0 = now_ms();
        ev("Дикарь %d: будит повара", id + 1);
        q_send(cook_q, M_COOK, 0, 0, 0);
    } else {
        ev("Дикарь %d: повар уже разбужен, ждёт без повторного запроса", id + 1);
    }
    enqueue(id);
}

static void handle_take(int id) {
    if (w.stopping) {
        reply(id, M_STOP);
        return;
    }
    ev("Дикарь %d: хочет есть, пытается взять порцию (в горшке %d/%d)", id + 1, w.pot, cfg.cap);
    attempt(id);
}

static void handle_fill(long want) {
    long portions = want;
    if (cfg.supply > 0 && portions > w.supply_left) portions = w.supply_left;
    if (w.pot != 0) fatal("повар пытается наполнить непустой горшок");
    w.pot = (int)portions;
    w.filled += portions;
    if (cfg.supply > 0) w.supply_left -= portions;
    w.fills++;
    w.cook_busy += now_ms() - w.cook_t0;
    w.pending = 0;
    if (cfg.supply > 0)
        ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d, остаток запаса %ld)",
           portions, w.pot, cfg.cap, w.supply_left);
    else
        ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d)", portions, w.pot, cfg.cap);
    ev("Повар: засыпает");
    for (long k = 0; k < portions; k++)
        if (!wake_first_waiter()) break;
}

static void do_stop(void) {
    w.stopping = 1;
    q_send(cook_q, M_STOP, 0, 0, 0);
    while (w.len > 0) {
        int id = w.wq[w.head];
        w.head = (w.head + 1) % MAX_SAVAGES;
        w.len--;
        reply(id, M_STOP);
    }
}

static void print_state(void) {
    ev("Состояние: в горшке %d/%d, ждут повара %d, повар %s, запросов %ld",
       w.pot, cfg.cap, w.len, w.pending ? "готовит" : "спит", w.requests);
}

static void print_stats(void) {
    int done = 0, starved = 0, stopped = 0, none = 0;
    for (int i = 0; i < cfg.n; i++) {
        if (w.st[i] == MS_DONE) done++;
        else if (w.st[i] == MS_STARVED) starved++;
        else if (w.st[i] == MS_STOPPED) stopped++;
        else none++;
    }
    out("\n===== Итог =====\n");
    if (w.stopping) out("Причина завершения: прервано пользователем\n");
    else if (starved) out("Причина завершения: запас порций исчерпан, ожидающие завершены корректно\n");
    else out("Причина завершения: все дикари выполнили заданное число приёмов пищи\n");
    out("Время работы: %ld мс\n", now_ms());
    out("Повар: наполнений=%ld, время работы=%ld мс, запросов=%ld\n", w.fills, w.cook_busy, w.requests);
    out("Горшок: опустошений=%ld, положено порций=%ld, взято порций=%ld, осталось=%d\n",
        w.empties, w.filled, w.taken, w.pot);
    if (cfg.supply > 0) out("Запас: было %ld, осталось %ld\n", cfg.supply, w.supply_left);
    out("Дикари: наелись=%d, ушли голодными=%d, прерваны=%d, не завершили=%d\n", done, starved, stopped, none);
    out("  №   порций  ждал, мс   статус\n");
    for (int i = 0; i < cfg.n; i++) {
        int st = w.st[i];
        const char *name = st == MS_DONE ? "наелся" : st == MS_STARVED ? "ушёл голодным"
                         : st == MS_STOPPED ? "прерван" : "не завершил работу";
        out(" %3d  %6d  %8ld   %s\n", i + 1, w.meals[i], w.wait_ms[i], name);
    }
}

static void cleanup(void) {
    if (pot_q != (mqd_t)-1) mq_close(pot_q);
    if (cook_q != (mqd_t)-1) mq_close(cook_q);
    mq_unlink(Q_POT);
    mq_unlink(Q_COOK);
    for (int i = 0; i < cfg.n; i++) {
        char name[48];
        snprintf(name, sizeof name, Q_REP, i);
        mq_close(rep_q[i]);
        mq_unlink(name);
    }
    if (ready_s) sem_close(ready_s);
    if (out_sem) sem_close(out_sem);
    sem_unlink(SEM_READY);
    sem_unlink(SEM_OUT);
}

static int setup(void) {
    for (int i = 0; i < MAX_SAVAGES; i++) rep_q[i] = (mqd_t)-1;
    sem_unlink(SEM_OUT);
    sem_unlink(SEM_READY);
    out_sem = sem_open(SEM_OUT, O_CREAT | O_EXCL | O_RDWR, 0600, 1);
    ready_s = sem_open(SEM_READY, O_CREAT | O_EXCL | O_RDWR, 0600, 0);
    if (out_sem == SEM_FAILED || ready_s == SEM_FAILED) {
        out("Ошибка sem_open: %s\n", strerror(errno));
        out_sem = ready_s = NULL;
        return -1;
    }
    pot_q = q_create(Q_POT, 10, O_RDWR);
    cook_q = q_create(Q_COOK, 10, O_WRONLY | O_NONBLOCK);
    if (pot_q == (mqd_t)-1 || cook_q == (mqd_t)-1) {
        out("Ошибка mq_open: %s\n", strerror(errno));
        return -1;
    }
    for (int i = 0; i < cfg.n; i++) {
        char name[48];
        snprintf(name, sizeof name, Q_REP, i);
        rep_q[i] = q_create(name, 2, O_WRONLY | O_NONBLOCK);
        if (rep_q[i] == (mqd_t)-1) {
            out("Ошибка mq_open: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int sem_timed(sem_t *s, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return sem_timedwait(s, &ts) == 0;
}

static void send_start(mqd_t q, long long base) {
    Msg m;
    memset(&m, 0, sizeof m);
    m.type = M_START;
    m.a = base;
    m.cfg = cfg;
    mq_send(q, (const char *)&m, sizeof m, 0);
}

int main(int argc, char **argv) {
    int rc = config_init(argc, argv);
    if (rc >= 0) return rc;

    signals_install();
    time_set_base(mono_ns());
    log_open(1);
    if (setup() < 0) {
        cleanup();
        return 1;
    }
    out_lock_hooks(out_lock, out_unlock);

    out("Seed=%u\n", cfg.seed);
    if (cfg.meals == 0 && cfg.supply == 0) out("Режим без ограничений: остановка по Ctrl+C\n");
    out("Ожидаем независимо запущенные процессы: cannibals_mq_cook и cannibals_mq_savage 1..%d\n", cfg.n);

    // барьер: начинаем, когда все готовы
    int ready = 0;
    while (ready < cfg.n + 1) {
        if (g_sig) {
            out("Прервано до начала обеда\n");
            cleanup();
            log_close();
            return 130;
        }
        if (sem_timed(ready_s, 200)) ready++;
    }
    long long base = mono_ns();
    time_set_base(base);
    send_start(cook_q, base);
    for (int i = 0; i < cfg.n; i++) send_start(rep_q[i], base);

    w.supply_left = cfg.supply;
    w.empties = 1;      // в начале горшок пуст
    ev("Начало обеда: дикарей=%d, вместимость=%d, приёмов=%d, запас=%s",
       cfg.n, cfg.cap, cfg.meals, cfg.supply > 0 ? "ограничен" : "не ограничен");

    int done = 0, stop_sent = 0, forced = 0;
    long deadline = 0;
    while (done < cfg.n) {
        if (g_usr1) {
            g_usr1 = 0;
            print_state();
        }
        if (g_sig >= 1 && !stop_sent) {
            stop_sent = 1;
            ev("Получен сигнал, останавливаем обед (повторный сигнал - немедленный выход)");
            do_stop();
            deadline = now_ms() + cfg.tmax + cfg.cmax + 2000;
        }
        if (g_sig >= 2) {
            forced = 1;
            out("Повторный сигнал, немедленный выход\n");
            break;
        }
        if (stop_sent && now_ms() > deadline) break;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        Msg m;
        if (mq_timedreceive(pot_q, (char *)&m, sizeof m, NULL, &ts) < 0) continue;
        switch (m.type) {
        case M_TAKE: handle_take(m.id); break;
        case M_FILL: handle_fill(m.a); break;
        case M_DONE:
            w.st[m.id] = (int)m.a;
            done++;
            break;
        default: break;
        }
        check();
    }
    if (!forced) q_send(cook_q, M_STOP, 0, 0, 0);
    print_stats();
    cleanup();
    log_close();
    return (g_sig || forced) ? 130 : 0;
}
