#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "common.h"
#include "net.h"

int net_set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

void net_tune(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
}

static int resolve(const char *addr, int port, int passive, struct addrinfo **res) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int e = getaddrinfo(addr, ps, &hints, res);
    if (e) out("Неверный адрес '%s': %s\n", addr, gai_strerror(e));
    return e;
}

int net_listen(const char *addr, int port) {
    struct addrinfo *ai;
    if (resolve(addr, port, 1, &ai)) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, 0);
    int one = 1;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || listen(fd, 64) < 0) {
        out("Не удалось слушать %s:%d: %s\n", addr, port, strerror(errno));
        if (fd >= 0) close(fd);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    net_set_nonblock(fd);
    return fd;
}

// повторяет попытки до wait_sec секунд: сервер мог ещё не успеть запуститься
int net_connect(const char *addr, int port, int wait_sec) {
    long long deadline = mono_ns() + (long long)wait_sec * 1000000000LL;
    for (;;) {
        struct addrinfo *ai;
        if (resolve(addr, port, 0, &ai)) return -1;
        int fd = socket(ai->ai_family, ai->ai_socktype, 0);
        int err = errno;
        if (fd >= 0) {
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                freeaddrinfo(ai);
                net_tune(fd);
                return fd;
            }
            err = errno;
            close(fd);
        }
        freeaddrinfo(ai);
        if (g_sig) return CC_SIGNAL;
        if (mono_ns() >= deadline) {
            out("Не удалось подключиться к %s:%d: %s\n", addr, port, strerror(err));
            return CC_FAIL;
        }
        msleep_raw(200);
    }
}

int net_send(int fd, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    const char *p = buf;
    size_t left = (size_t)n;
    while (left > 0) {
        ssize_t k = send(fd, p, left, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += k;
        left -= (size_t)k;
    }
    return 0;
}

void lr_init(LineReader *r, int fd) {
    r->fd = fd;
    r->len = 0;
}

// ждёт строку от сервера; timeout_ms < 0 - без ограничения
int lr_wait(LineReader *r, char *line, size_t cap, long timeout_ms) {
    long long deadline = timeout_ms < 0 ? 0 : mono_ns() / 1000000LL + timeout_ms;
    for (;;) {
        char *nl = memchr(r->buf, '\n', (size_t)r->len);
        if (nl) {
            size_t n = (size_t)(nl - r->buf);
            size_t c = n < cap - 1 ? n : cap - 1;
            memcpy(line, r->buf, c);
            line[c] = '\0';
            if (c > 0 && line[c - 1] == '\r') line[c - 1] = '\0';
            memmove(r->buf, nl + 1, (size_t)r->len - n - 1);
            r->len -= (int)n + 1;
            return W_LINE;
        }
        if (g_sig) return W_SIG;
        long slice = 200;
        if (timeout_ms >= 0) {
            long left = (long)(deadline - mono_ns() / 1000000LL);
            if (left <= 0) return W_TIMEOUT;
            if (left < slice) slice = left;
        }
        struct pollfd p = { r->fd, POLLIN, 0 };
        int pr = poll(&p, 1, (int)slice);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return W_EOF;
        }
        if (pr > 0) {
            if (r->len >= (int)sizeof r->buf) return W_EOF;
            ssize_t k = read(r->fd, r->buf + r->len, sizeof r->buf - (size_t)r->len);
            if (k == 0) return W_EOF;
            if (k < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return W_EOF;
            }
            r->len += (int)k;
        }
    }
}

static void client_usage(const char *prog, int need_id) {
    out("Использование: %s [-a ADDR] [-p PORT]%s [-w СЕК] [-l FILE]\n"
        "  -a ADDR  адрес сервера (по умолчанию 127.0.0.1)\n"
        "  -p PORT  порт сервера (по умолчанию 5555)\n"
        "%s"
        "  -w СЕК   сколько секунд пытаться подключиться (по умолчанию 10)\n"
        "  -l FILE  журнал (по умолчанию не ведётся)\n",
        prog, need_id ? " -i ID" : "",
        need_id ? "  -i ID    номер дикаря, 1..100\n" : "");
}

int client_parse(int argc, char **argv, ClientOpts *o, int need_id) {
    strcpy(o->addr, "127.0.0.1");
    o->port = 5555;
    o->id = 0;
    o->wait_sec = 10;
    o->log_path[0] = '\0';
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0' || a[2] != '\0') {
            client_usage(argv[0], need_id);
            return 1;
        }
        if (a[1] == 'h') {
            client_usage(argv[0], need_id);
            return 0;
        }
        if (i + 1 >= argc) {
            out("Ошибка: у опции -%c нет значения\n", a[1]);
            return 1;
        }
        const char *v = argv[++i];
        char *end = NULL;
        long x = strtol(v, &end, 10);
        int numeric = *v != '\0' && *end == '\0';
        switch (a[1]) {
        case 'a':
            if (strlen(v) == 0 || strlen(v) >= sizeof o->addr) { out("Ошибка: неверный адрес\n"); return 1; }
            strcpy(o->addr, v);
            break;
        case 'p':
            if (!numeric || x < 1 || x > 65535) { out("Ошибка: неверный порт '%s'\n", v); return 1; }
            o->port = (int)x;
            break;
        case 'i':
            if (!need_id || !numeric || x < 1 || x > MAX_SAVAGES) { out("Ошибка: неверный номер дикаря '%s'\n", v); return 1; }
            o->id = (int)x;
            break;
        case 'w':
            if (!numeric || x < 0 || x > 3600) { out("Ошибка: неверное время ожидания '%s'\n", v); return 1; }
            o->wait_sec = (int)x;
            break;
        case 'l':
            snprintf(o->log_path, sizeof o->log_path, "%s", v);
            break;
        default:
            client_usage(argv[0], need_id);
            return 1;
        }
    }
    if (need_id && o->id == 0) {
        out("Ошибка: не указан номер дикаря (-i ID)\n");
        return 1;
    }
    return -1;
}

static const char *reject_text(const char *why) {
    if (!strcmp(why, "busy")) return "эта роль уже занята другим подключением";
    if (!strcmp(why, "bad_id")) return "номер дикаря вне диапазона сервера";
    if (!strcmp(why, "stopping")) return "сервер останавливается";
    return why;
}

// подключается, здоровается и читает параметры; возвращает сокет или CC_*
int client_connect(const ClientOpts *o, const char *hello, LineReader *lr, int *done) {
    int fd = net_connect(o->addr, o->port, o->wait_sec);
    if (fd < 0) return fd;
    lr_init(lr, fd);
    net_send(fd, "%s\n", hello);
    char line[256];
    int r = lr_wait(lr, line, sizeof line, 5000);
    if (r == W_SIG) {
        close(fd);
        return CC_SIGNAL;
    }
    if (r != W_LINE) {
        out("Сервер не ответил на приветствие\n");
        close(fd);
        return CC_FAIL;
    }
    if (strncmp(line, "REJECT ", 7) == 0) {
        close(fd);
        if (!strcmp(line + 7, "finished")) {
            out("Сервер: этот дикарь уже закончил питание\n");
            return CC_FINISHED;
        }
        out("Сервер отклонил подключение: %s\n", reject_text(line + 7));
        return CC_FAIL;
    }
    if (client_is_shutdown(line)) {
        close(fd);
        return CC_FINISHED;
    }
    long ms = 0;
    int dn = 0;
    if (sscanf(line, "WELCOME %d %d %d %ld %ld %ld %ld %ld %u %d %ld", &cfg.n, &cfg.cap, &cfg.meals,
               &cfg.tmin, &cfg.tmax, &cfg.cmin, &cfg.cmax, &cfg.supply, &cfg.seed, &dn, &ms) != 11) {
        out("Непонятный ответ сервера: %s\n", line);
        close(fd);
        return CC_FAIL;
    }
    time_set_base(mono_ns() - (long long)ms * 1000000LL);
    *done = dn;
    return fd;
}

int client_is_shutdown(const char *line) {
    if (strncmp(line, "SHUTDOWN", 8) != 0) return 0;
    if (strstr(line, "stopped")) ev("Сервер остановлен пользователем, клиент отключается");
    else ev("Сервер завершил работу, клиент отключается");
    return 1;
}
