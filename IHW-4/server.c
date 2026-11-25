// Сервер: владеет состоянием горшка, принимает подключения повара, дикарей и наблюдателей.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "net.h"

#define MAX_CONNS 256
#define OUT_LIMIT (64 * 1024)
#define HIST 30

enum { K_NEW, K_COOK, K_SAVAGE, K_OBS };
enum { SV_NONE, SV_ACTIVE, SV_DONE, SV_STARVED };

typedef struct {
    int fd, kind, id, dead;
    long t_open;
    char in[1024];
    int inlen;
    char *out;
    size_t olen, ocap;
} Conn;

static Conn cn[MAX_CONNS];
static int savage_conn[MAX_SAVAGES];
static int cook_conn = -1;
static int listen_fd = -1;

static struct {
    int pot, pending, cook_busy, stopping;
    long supply_left, empties, requests, filled, taken, fills, cook_ms, cook_t0;
    int wq[MAX_SAVAGES], head, len;
    int inq[MAX_SAVAGES], meals[MAX_SAVAGES], st[MAX_SAVAGES];
    long wait_ms[MAX_SAVAGES], wait_t0[MAX_SAVAGES];
} w;

static char hist[HIST][340];
static int hist_head, hist_len;

// ---------- отправка ----------

static void flush_conn(Conn *c) {
    while (c->olen > 0) {
        ssize_t k = send(c->fd, c->out, c->olen, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c->dead = 1;
            return;
        }
        memmove(c->out, c->out + k, c->olen - (size_t)k);
        c->olen -= (size_t)k;
    }
}

static void conn_write(Conn *c, const char *s, size_t n) {
    if (c->fd < 0 || c->dead) return;
    // медленный получатель не должен тормозить сервер
    if (c->olen + n > OUT_LIMIT) {
        c->dead = 1;
        return;
    }
    if (c->olen + n > c->ocap) {
        size_t nc = c->ocap ? c->ocap * 2 : 512;
        while (nc < c->olen + n) nc *= 2;
        char *p = realloc(c->out, nc);
        if (!p) {
            c->dead = 1;
            return;
        }
        c->out = p;
        c->ocap = nc;
    }
    memcpy(c->out + c->olen, s, n);
    c->olen += n;
    flush_conn(c);
}

static void vsend(Conn *c, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    conn_write(c, buf, (size_t)n);
}

static void sendf(Conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sendf(Conn *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsend(c, fmt, ap);
    va_end(ap);
}

static void to_savage(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void to_savage(int id, const char *fmt, ...) {
    if (savage_conn[id] < 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsend(&cn[savage_conn[id]], fmt, ap);
    va_end(ap);
}

static void to_cook(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void to_cook(const char *fmt, ...) {
    if (cook_conn < 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsend(&cn[cook_conn], fmt, ap);
    va_end(ap);
}

static int count_kind(int kind) {
    int k = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (cn[i].fd >= 0 && !cn[i].dead && cn[i].kind == kind) k++;
    return k;
}

// событие: на экран сервера и всем наблюдателям
static void say(int stamped, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int stamped, const char *fmt, ...) {
    char text[300], line[340];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    long ms = now_ms();
    if (stamped) {
        out("[%7.3f] %s\n", (double)ms / 1000.0, text);
        snprintf(line, sizeof line, "EV %ld %s\n", ms, text);
        int idx = hist_len < HIST ? (hist_head + hist_len++) % HIST : hist_head;
        if (hist_len == HIST && idx == hist_head) hist_head = (hist_head + 1) % HIST;
        snprintf(hist[idx], sizeof hist[idx], "%s", line);
    } else {
        out("%s\n", text);
        snprintf(line, sizeof line, "TXT %s\n", text);
    }
    for (int i = 0; i < MAX_CONNS; i++)
        if (cn[i].fd >= 0 && cn[i].kind == K_OBS) conn_write(&cn[i], line, strlen(line));
}

// ---------- модель ----------

static void fatal(const char *msg) {
    out("НАРУШЕН ИНВАРИАНТ: %s\nАварийное завершение.\n", msg);
    _exit(2);
}

static void check(void) {
    if (w.pot < 0 || w.pot > cfg.cap) fatal("число порций вне [0, вместимость]");
    if (w.filled - w.taken != w.pot) fatal("баланс порций не сходится");
    if (w.requests > w.empties) fatal("запросов больше, чем опустошений");
}

static void enqueue(int id) {
    w.wq[(w.head + w.len) % MAX_SAVAGES] = id;
    w.len++;
    w.inq[id] = 1;
    w.wait_t0[id] = now_ms();
}

static void remove_waiter(int id) {
    if (!w.inq[id]) return;
    int k = 0;
    for (int i = 0; i < w.len; i++) {
        int v = w.wq[(w.head + i) % MAX_SAVAGES];
        if (v != id) w.wq[(w.head + k++) % MAX_SAVAGES] = v;
    }
    w.len = k;
    w.inq[id] = 0;
}

static int online(int id) {
    int ci = savage_conn[id];
    return ci >= 0 && !cn[ci].dead;
}

static void call_cook(void) {
    if (cook_conn < 0 || w.cook_busy) return;
    w.cook_busy = 1;
    w.cook_t0 = now_ms();
    to_cook("COOK\n");
}

static void attempt(int id);

// будит первого ожидающего, который ещё подключён
static int wake_first_waiter(void) {
    while (w.len > 0) {
        int id = w.wq[w.head];
        w.head = (w.head + 1) % MAX_SAVAGES;
        w.len--;
        w.inq[id] = 0;
        if (!online(id)) continue;
        w.wait_ms[id] += now_ms() - w.wait_t0[id];
        attempt(id);
        return 1;
    }
    return 0;
}

static void attempt(int id) {
    if (w.pot > 0) {
        w.pot--;
        w.taken++;
        w.meals[id]++;
        say(1, "Дикарь %d: получил порцию #%d, в горшке осталось %d", id + 1, w.meals[id], w.pot);
        to_savage(id, "GRANT %d\n", w.meals[id]);
        if (cfg.meals > 0 && w.meals[id] >= cfg.meals) {
            w.st[id] = SV_DONE;
            say(1, "Дикарь %d: завершил питание (съел %d порц.)", id + 1, w.meals[id]);
        }
        if (w.pot == 0) {
            w.empties++;
            wake_first_waiter();    // он обнаружит пустой горшок
        }
        return;
    }
    say(1, "Дикарь %d: обнаружил пустой горшок", id + 1);
    if (cfg.supply > 0 && w.supply_left == 0) {
        w.st[id] = SV_STARVED;
        say(1, "Дикарь %d: запас исчерпан, уходит голодным (съел %d порц.)", id + 1, w.meals[id]);
        to_savage(id, "STARVED\n");
        wake_first_waiter();        // цепочка: следующий ожидающий тоже узнает
        return;
    }
    if (!w.pending) {
        w.pending = 1;
        w.requests++;
        say(1, "Дикарь %d: будит повара", id + 1);
        if (cook_conn >= 0) call_cook();
        else say(1, "Повар не подключён, запрос ждёт его появления");
    } else {
        say(1, "Дикарь %d: повар уже разбужен, ждёт без повторного запроса", id + 1);
    }
    enqueue(id);
}

static void handle_take(int id) {
    if (w.inq[id] || w.st[id] != SV_ACTIVE) return;
    say(1, "Дикарь %d: хочет есть, пытается взять порцию (в горшке %d/%d)", id + 1, w.pot, cfg.cap);
    attempt(id);
}

static void handle_fill(long want) {
    if (!w.pending || !w.cook_busy) return;
    w.cook_busy = 0;
    long portions = want < cfg.cap ? want : cfg.cap;
    if (portions < 0) portions = 0;
    if (cfg.supply > 0 && portions > w.supply_left) portions = w.supply_left;
    if (w.pot != 0) fatal("повар пытается наполнить непустой горшок");
    w.pot = (int)portions;
    w.filled += portions;
    if (cfg.supply > 0) w.supply_left -= portions;
    w.fills++;
    w.cook_ms += now_ms() - w.cook_t0;
    w.pending = 0;
    if (cfg.supply > 0)
        say(1, "Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d, остаток запаса %ld)",
            portions, w.pot, cfg.cap, w.supply_left);
    else
        say(1, "Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d)", portions, w.pot, cfg.cap);
    say(1, "Повар: засыпает");
    for (long k = 0; k < portions; k++)
        if (!wake_first_waiter()) break;
}

static void print_state(void) {
    say(1, "Состояние: в горшке %d/%d, ждут %d, повар %s, дикарей онлайн %d/%d, наблюдателей %d, запросов %ld",
        w.pot, cfg.cap, w.len,
        cook_conn < 0 ? "не подключён" : w.cook_busy ? "готовит" : "спит",
        count_kind(K_SAVAGE), cfg.n, count_kind(K_OBS), w.requests);
}

// ---------- подключения ----------

static void welcome(Conn *c, int done) {
    sendf(c, "WELCOME %d %d %d %ld %ld %ld %ld %ld %u %d %ld\n", cfg.n, cfg.cap, cfg.meals,
          cfg.tmin, cfg.tmax, cfg.cmin, cfg.cmax, cfg.supply, cfg.seed, done, now_ms());
}

static void reject(Conn *c, const char *why) {
    sendf(c, "REJECT %s\n", why);
    c->dead = 1;
}

static void handle_hello(int ci, const char *line) {
    Conn *c = &cn[ci];
    char role[16] = "";
    int id = 0;
    sscanf(line, "HELLO %15s %d", role, &id);
    if (!strcmp(role, "COOK")) {
        if (cook_conn >= 0) return reject(c, "busy");
        c->kind = K_COOK;
        cook_conn = ci;
        welcome(c, 0);
        say(1, "Повар: подключился");
        if (w.pending && !w.cook_busy) {
            say(1, "Повар: есть невыполненный запрос, приступает к приготовлению");
            call_cook();
        }
    } else if (!strcmp(role, "SAVAGE")) {
        if (id < 1 || id > cfg.n) return reject(c, "bad_id");
        int i = id - 1;
        if (w.st[i] == SV_DONE || w.st[i] == SV_STARVED) return reject(c, "finished");
        if (w.stopping) return reject(c, "stopping");
        if (savage_conn[i] >= 0) return reject(c, "busy");
        int again = w.st[i] == SV_ACTIVE;
        c->kind = K_SAVAGE;
        c->id = i;
        savage_conn[i] = ci;
        w.st[i] = SV_ACTIVE;
        welcome(c, w.meals[i]);
        if (again) say(1, "Дикарь %d: подключился снова (съел ранее %d порц.)", id, w.meals[i]);
        else say(1, "Дикарь %d: подключился", id);
    } else if (!strcmp(role, "OBSERVER")) {
        welcome(c, 0);
        sendf(c, "TXT --- наблюдатель подключён: в горшке %d/%d, ждут %d, повар %s, дикарей онлайн %d/%d ---\n",
              w.pot, cfg.cap, w.len, cook_conn < 0 ? "не подключён" : w.cook_busy ? "готовит" : "спит",
              count_kind(K_SAVAGE), cfg.n);
        for (int k = 0; k < hist_len; k++) conn_write(c, hist[(hist_head + k) % HIST], strlen(hist[(hist_head + k) % HIST]));
        sendf(c, "TXT --- далее события в реальном времени ---\n");
        c->kind = K_OBS;
        say(1, "Наблюдатель подключился (всего %d)", count_kind(K_OBS));
    } else {
        c->dead = 1;
    }
}

static void handle_line(int ci, const char *line) {
    Conn *c = &cn[ci];
    if (c->kind == K_NEW) {
        if (!strncmp(line, "HELLO ", 6)) handle_hello(ci, line);
        else c->dead = 1;
        return;
    }
    if (c->kind == K_SAVAGE && !strcmp(line, "TAKE")) return handle_take(c->id);
    if (c->kind == K_COOK && !strncmp(line, "FILL ", 5)) return handle_fill(atol(line + 5));
    c->dead = 1;    // BYE и всё неизвестное
}

static void drop_conn(int ci) {
    Conn c = cn[ci];
    free(cn[ci].out);
    memset(&cn[ci], 0, sizeof cn[ci]);
    cn[ci].fd = -1;
    close(c.fd);
    if (c.kind == K_SAVAGE) {
        savage_conn[c.id] = -1;
        remove_waiter(c.id);
        if (w.st[c.id] == SV_ACTIVE)
            say(1, "Дикарь %d: отключился (съел %d порц.), может подключиться снова", c.id + 1, w.meals[c.id]);
    } else if (c.kind == K_COOK) {
        cook_conn = -1;
        if (w.cook_busy) {
            w.cook_busy = 0;
            say(1, "Повар: отключился во время приготовления, запрос остаётся в силе");
        } else {
            say(1, "Повар: отключился");
        }
    } else if (c.kind == K_OBS) {
        say(1, "Наблюдатель отключился (осталось %d)", count_kind(K_OBS));
    }
}

static void read_conn(int ci) {
    Conn *c = &cn[ci];
    ssize_t k = read(c->fd, c->in + c->inlen, sizeof c->in - (size_t)c->inlen - 1);
    if (k == 0) {
        c->dead = 1;
        return;
    }
    if (k < 0) {
        if (errno != EAGAIN && errno != EINTR) c->dead = 1;
        return;
    }
    c->inlen += (int)k;
    char *start = c->in, *nl;
    while (!c->dead && (nl = memchr(start, '\n', (size_t)(c->in + c->inlen - start)))) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r') nl[-1] = '\0';
        handle_line(ci, start);
        start = nl + 1;
    }
    c->inlen -= (int)(start - c->in);
    memmove(c->in, start, (size_t)c->inlen);
    if (c->inlen >= (int)sizeof c->in - 1) c->dead = 1;
}

static void accept_new(void) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < MAX_CONNS && slot < 0; i++)
            if (cn[i].fd < 0) slot = i;
        if (slot < 0) {
            close(fd);
            continue;
        }
        net_set_nonblock(fd);
        net_tune(fd);
        memset(&cn[slot], 0, sizeof cn[slot]);
        cn[slot].fd = fd;
        cn[slot].kind = K_NEW;
        cn[slot].t_open = now_ms();
    }
}

// ---------- завершение и итог ----------

static int all_done(void) {
    for (int i = 0; i < cfg.n; i++)
        if (w.st[i] != SV_DONE && w.st[i] != SV_STARVED) return 0;
    return 1;
}

static void print_stats(void) {
    int done = 0, starved = 0, other = 0, none = 0;
    for (int i = 0; i < cfg.n; i++) {
        if (w.st[i] == SV_DONE) done++;
        else if (w.st[i] == SV_STARVED) starved++;
        else if (w.st[i] == SV_NONE) none++;
        else other++;
    }
    say(0, "%s", "");
    say(0, "===== Итог =====");
    if (w.stopping) say(0, "Причина завершения: прервано пользователем");
    else if (starved) say(0, "Причина завершения: запас порций исчерпан, ожидающие завершены корректно");
    else say(0, "Причина завершения: все дикари выполнили заданное число приёмов пищи");
    say(0, "Время работы: %ld мс", now_ms());
    say(0, "Повар: наполнений=%ld, время работы=%ld мс, запросов=%ld", w.fills, w.cook_ms, w.requests);
    say(0, "Горшок: опустошений=%ld, положено порций=%ld, взято порций=%ld, осталось=%d",
        w.empties, w.filled, w.taken, w.pot);
    if (cfg.supply > 0) say(0, "Запас: было %ld, осталось %ld", cfg.supply, w.supply_left);
    say(0, "Дикари: наелись=%d, ушли голодными=%d, прерваны=%d, не подключались=%d", done, starved, other, none);
    say(0, "  №   порций  ждал, мс   статус");
    for (int i = 0; i < cfg.n; i++) {
        const char *name = w.st[i] == SV_DONE ? "наелся" : w.st[i] == SV_STARVED ? "ушёл голодным"
                         : w.st[i] == SV_NONE ? "не подключался" : "прерван";
        say(0, " %3d  %6d  %8ld   %s", i + 1, w.meals[i], w.wait_ms[i], name);
    }
}

static void finish(void) {
    print_stats();
    for (int i = 0; i < MAX_CONNS; i++)
        if (cn[i].fd >= 0)
            sendf(&cn[i], "SHUTDOWN %s\n", w.stopping ? "stopped" : "normal");
    // даём время дослать оставшееся
    long long deadline = mono_ns() + 1500000000LL;
    while (mono_ns() < deadline && g_sig < 2) {
        struct pollfd pf[MAX_CONNS];
        int n = 0;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (cn[i].fd >= 0 && !cn[i].dead && cn[i].olen > 0) {
                pf[n].fd = cn[i].fd;
                pf[n].events = POLLOUT;
                pf[n].revents = 0;
                n++;
            }
        }
        if (n == 0) break;
        poll(pf, (nfds_t)n, 100);
        for (int i = 0; i < MAX_CONNS; i++)
            if (cn[i].fd >= 0 && cn[i].olen > 0) flush_conn(&cn[i]);
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        if (cn[i].fd >= 0) {
            close(cn[i].fd);
            free(cn[i].out);
            cn[i].fd = -1;
        }
    }
    close(listen_fd);
}

int main(int argc, char **argv) {
    int rc = config_init(argc, argv);
    if (rc >= 0) return rc;

    signals_install();
    log_open(cfg.log_path);
    for (int i = 0; i < MAX_CONNS; i++) cn[i].fd = -1;
    for (int i = 0; i < MAX_SAVAGES; i++) savage_conn[i] = -1;
    listen_fd = net_listen(cfg.addr, cfg.port);
    if (listen_fd < 0) {
        log_close();
        return 1;
    }
    time_set_base(mono_ns());
    w.supply_left = cfg.supply;
    w.empties = 1;      // в начале горшок пуст

    out("Seed=%u\n", cfg.seed);
    out("Сервер слушает %s:%d, ожидаем: повар, дикари 1..%d, наблюдатели\n", cfg.addr, cfg.port, cfg.n);
    if (cfg.meals == 0 && cfg.supply == 0) out("Режим без ограничений: остановка по Ctrl+C\n");
    say(1, "Начало обеда: дикарей=%d, вместимость=%d, приёмов=%d, запас=%s",
        cfg.n, cfg.cap, cfg.meals, cfg.supply > 0 ? "ограничен" : "не ограничен");

    while (!all_done()) {
        if (g_usr1) {
            g_usr1 = 0;
            print_state();
        }
        if (g_sig) {
            w.stopping = 1;
            say(1, "Получен сигнал, останавливаем обед");
            break;
        }
        struct pollfd pf[MAX_CONNS + 1];
        int map[MAX_CONNS + 1], n = 0;
        pf[n].fd = listen_fd;
        pf[n].events = POLLIN;
        map[n++] = -1;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (cn[i].fd < 0) continue;
            pf[n].fd = cn[i].fd;
            pf[n].events = POLLIN | (cn[i].olen > 0 ? POLLOUT : 0);
            map[n++] = i;
        }
        int pr = poll(pf, (nfds_t)n, 200);
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0) {
            for (int k = 0; k < n; k++) {
                if (!pf[k].revents) continue;
                if (map[k] < 0) {
                    accept_new();
                    continue;
                }
                Conn *c = &cn[map[k]];
                if (c->fd < 0) continue;
                if (pf[k].revents & POLLOUT) flush_conn(c);
                if (pf[k].revents & (POLLIN | POLLHUP | POLLERR)) read_conn(map[k]);
            }
        }
        for (int i = 0; i < MAX_CONNS; i++) {
            if (cn[i].fd < 0) continue;
            if (cn[i].kind == K_NEW && now_ms() - cn[i].t_open > 5000) cn[i].dead = 1;
            if (cn[i].dead) drop_conn(i);
        }
        check();
    }

    finish();
    log_close();
    return w.stopping ? 130 : 0;
}
