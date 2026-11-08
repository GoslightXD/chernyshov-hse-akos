#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "common.h"

Config cfg;
volatile sig_atomic_t g_sig;
volatile sig_atomic_t g_usr1;

static int log_fd = -1;
static void (*lock_hook)(void);
static void (*unlock_hook)(void);
static int (*stop_hook)(void);
static long long t_base;

static void write_all(int fd, const char *p, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, p, len);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += r;
        len -= (size_t)r;
    }
}

// каждая строка уходит одним write, поэтому строки разных процессов не смешиваются
static void emit(const char *buf, size_t n) {
    if (lock_hook) lock_hook();
    write_all(STDOUT_FILENO, buf, n);
    if (log_fd >= 0) write_all(log_fd, buf, n);
    if (unlock_hook) unlock_hook();
}

void out_lock_hooks(void (*lock)(void), void (*unlock)(void)) {
    lock_hook = lock;
    unlock_hook = unlock;
}

void out(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    emit(buf, (size_t)n);
}

void ev(const char *fmt, ...) {
    char msg[400], line[460];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof line, "[%7.3f] %s\n", (double)now_ms() / 1000.0, msg);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    emit(line, (size_t)n);
}

void log_open(int truncate) {
    if (!cfg.log_path[0]) return;
    int flags = O_WRONLY | O_CREAT | O_APPEND | (truncate ? O_TRUNC : 0);
    log_fd = open(cfg.log_path, flags, 0644);
    if (log_fd < 0) out("Предупреждение: не удалось открыть журнал '%s'\n", cfg.log_path);
}

void log_close(void) {
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
}

long long mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

void time_set_base(long long ns) { t_base = ns; }

long now_ms(void) { return (long)((mono_ns() - t_base) / 1000000LL); }

void msleep_raw(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void set_stop_hook(int (*fn)(void)) { stop_hook = fn; }

static int stop_now(void) { return g_sig || (stop_hook && stop_hook()); }

// спит порциями, чтобы быстро заметить остановку
void sleep_ms(long ms) {
    while (ms > 0 && !stop_now()) {
        long step = ms < 20 ? ms : 20;
        msleep_raw(step);
        ms -= step;
    }
}

long rnd(unsigned *state, long a, long b) {
    if (b <= a) return a;
    return a + (long)(rand_r(state) % (unsigned)(b - a + 1));
}

static void on_signal(int sig) {
    if (sig == SIGUSR1) g_usr1 = 1;
    else g_sig++;
}

// без SA_RESTART, чтобы блокирующие вызовы прерывались с EINTR
void signals_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
}

static int read_line(char *buf, size_t size) {
    size_t i = 0;
    while (i + 1 < size) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (i == 0) return 0;
            break;
        }
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return 1;
}

static long ask_long(const char *prompt, long lo, long hi, long def) {
    char line[64];
    for (;;) {
        out("%s [%ld..%ld, по умолчанию %ld]: ", prompt, lo, hi, def);
        if (!read_line(line, sizeof line)) {
            out("\n");
            return def;
        }
        if (line[0] == '\0') return def;
        char *end;
        long v = strtol(line, &end, 10);
        if (*end == '\0' && v >= lo && v <= hi) return v;
        out("  Некорректное значение, повторите ввод.\n");
    }
}

static void defaults(void) {
    cfg.n = 5;
    cfg.cap = 4;
    cfg.meals = 3;
    cfg.tmin = 300;
    cfg.tmax = 1200;
    cfg.cmin = 500;
    cfg.cmax = 1500;
    cfg.supply = 0;
    cfg.seed = (unsigned)time(NULL);
    strcpy(cfg.log_path, "cannibals.log");
}

static void usage(const char *prog) {
    out("Использование: %s [опции]\n"
        "  -n N        количество дикарей (1..%d)\n"
        "  -c M        вместимость горшка\n"
        "  -m K        приёмов пищи у каждого (0 = бесконечно, остановка по Ctrl+C)\n"
        "  -t MIN:MAX  пауза между приёмами пищи, мс\n"
        "  -k MIN:MAX  длительность приготовления, мс\n"
        "  -s S        общий запас порций (0 = не ограничен)\n"
        "  -r SEED     начальное значение генератора случайных чисел\n"
        "  -l FILE     файл журнала (по умолчанию cannibals.log, -l '' отключает)\n"
        "  -i          интерактивный ввод (также при запуске без аргументов)\n"
        "  -h          справка\n"
        "Сигналы: SIGINT/SIGTERM - остановка, SIGUSR1 - вывести состояние,\n"
        "повторный SIGINT - немедленный выход.\n",
        prog, MAX_SAVAGES);
}

static void interactive(void) {
    out("Параметры (Enter - значение по умолчанию)\n");
    cfg.n = (int)ask_long("Количество дикарей", 1, MAX_SAVAGES, cfg.n);
    cfg.cap = (int)ask_long("Вместимость горшка", 1, 1000000, cfg.cap);
    cfg.meals = (int)ask_long("Приёмов пищи у каждого (0 = бесконечно)", 0, 1000000, cfg.meals);
    cfg.tmin = ask_long("Мин. пауза между приёмами, мс", 0, 100000, cfg.tmin);
    cfg.tmax = ask_long("Макс. пауза между приёмами, мс", cfg.tmin, 100000, cfg.tmax < cfg.tmin ? cfg.tmin : cfg.tmax);
    cfg.cmin = ask_long("Мин. время приготовления, мс", 0, 100000, cfg.cmin);
    cfg.cmax = ask_long("Макс. время приготовления, мс", cfg.cmin, 100000, cfg.cmax < cfg.cmin ? cfg.cmin : cfg.cmax);
    cfg.supply = ask_long("Общий запас порций (0 = не ограничен)", 0, 1000000000L, cfg.supply);
    cfg.seed = (unsigned)ask_long("Seed", 0, 2147483647L, (long)(cfg.seed % 2147483647u));
}

static int parse_long(const char *s, long *v) {
    char *e;
    errno = 0;
    *v = strtol(s, &e, 10);
    return *s != '\0' && *e == '\0' && errno == 0;
}

static int parse_int(const char *s, int *v) {
    long x;
    if (!parse_long(s, &x) || x < -2000000000L || x > 2000000000L) return 0;
    *v = (int)x;
    return 1;
}

static int parse_range(const char *s, long *a, long *b) {
    return sscanf(s, "%ld:%ld", a, b) == 2 && *a >= 0 && *b >= *a;
}

static int validate(void) {
    if (cfg.n < 1 || cfg.n > MAX_SAVAGES) { out("Ошибка: число дикарей должно быть 1..%d\n", MAX_SAVAGES); return 0; }
    if (cfg.cap < 1) { out("Ошибка: вместимость горшка должна быть >= 1\n"); return 0; }
    if (cfg.meals < 0) { out("Ошибка: число приёмов пищи не может быть отрицательным\n"); return 0; }
    if (cfg.tmin < 0 || cfg.tmax < cfg.tmin) { out("Ошибка: неверный диапазон пауз между приёмами\n"); return 0; }
    if (cfg.cmin < 0 || cfg.cmax < cfg.cmin) { out("Ошибка: неверный диапазон времени приготовления\n"); return 0; }
    if (cfg.supply < 0) { out("Ошибка: запас порций не может быть отрицательным\n"); return 0; }
    return 1;
}

int config_init(int argc, char **argv) {
    defaults();
    int want_interactive = (argc == 1);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0' || a[2] != '\0') {
            usage(argv[0]);
            return 1;
        }
        char opt = a[1];
        if (opt == 'h') {
            usage(argv[0]);
            return 0;
        }
        if (opt == 'i') {
            want_interactive = 1;
            continue;
        }
        if (i + 1 >= argc) {
            out("Ошибка: у опции -%c нет значения\n", opt);
            return 1;
        }
        const char *v = argv[++i];
        long x = 0;
        int ok = 1;
        switch (opt) {
        case 'n': ok = parse_int(v, &cfg.n); break;
        case 'c': ok = parse_int(v, &cfg.cap); break;
        case 'm': ok = parse_int(v, &cfg.meals); break;
        case 's': ok = parse_long(v, &cfg.supply); break;
        case 'r': ok = parse_long(v, &x) && x >= 0; cfg.seed = (unsigned)x; break;
        case 't': ok = parse_range(v, &cfg.tmin, &cfg.tmax); break;
        case 'k': ok = parse_range(v, &cfg.cmin, &cfg.cmax); break;
        case 'l': snprintf(cfg.log_path, sizeof cfg.log_path, "%s", v); break;
        default:
            usage(argv[0]);
            return 1;
        }
        if (!ok) {
            out("Ошибка: неверное значение '%s' для опции -%c\n", v, opt);
            return 1;
        }
    }

    if (want_interactive) interactive();
    if (!validate()) return 1;
    return -1;
}
