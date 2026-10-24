#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

Config cfg;

static int log_fd = -1;
static pthread_mutex_t out_m = PTHREAD_MUTEX_INITIALIZER;
static char obuf[1 << 16];
static size_t olen;
static struct timespec t_start;

static void write_all(int fd, const char *p, size_t len) {
  while (len > 0) {
    ssize_t r = write(fd, p, len);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    p += r;
    len -= (size_t)r;
  }
}

static void flush_locked(void) {
  if (olen == 0)
    return;
  write_all(STDOUT_FILENO, obuf, olen);
  if (log_fd >= 0)
    write_all(log_fd, obuf, olen);
  olen = 0;
}

static void append_locked(const char *s, size_t n) {
  if (olen + n > sizeof obuf)
    flush_locked();
  memcpy(obuf + olen, s, n);
  olen += n;
}

void flush_out(void) {
  pthread_mutex_lock(&out_m);
  flush_locked();
  pthread_mutex_unlock(&out_m);
}

void out(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof buf)
    n = (int)sizeof buf - 1;
  pthread_mutex_lock(&out_m);
  append_locked(buf, (size_t)n);
  pthread_mutex_unlock(&out_m);
}

// строка с отметкой времени; время берётся под замком, чтобы порядок строк
// совпадал с порядком времени
void ev(const char *fmt, ...) {
  char msg[400], line[460];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&out_m);
  int n = snprintf(line, sizeof line, "[%7.3f] %s\n", (double)now_ms() / 1000.0,
                   msg);
  if (n < 0)
    n = 0;
  if ((size_t)n >= sizeof line)
    n = (int)sizeof line - 1;
  append_locked(line, (size_t)n);
  pthread_mutex_unlock(&out_m);
}

void log_open(void) {
  flush_out();
  if (!cfg.log_path[0])
    return;
  log_fd = open(cfg.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (log_fd < 0)
    out("Предупреждение: не удалось открыть журнал '%s'\n", cfg.log_path);
}

void log_close(void) {
  if (log_fd >= 0)
    close(log_fd);
  log_fd = -1;
}

void time_start(void) { clock_gettime(CLOCK_MONOTONIC, &t_start); }

long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)((t.tv_sec - t_start.tv_sec) * 1000L +
                (t.tv_nsec - t_start.tv_nsec) / 1000000L);
}

// спит порциями, чтобы быстро заметить остановку
void sleep_ms(long ms) {
  if (ms >= 10)
    flush_out();
  while (ms > 0 && !atomic_load(&g_stop)) {
    long step = ms < 20 ? ms : 20;
    struct timespec ts = {step / 1000, (step % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    ms -= step;
  }
}

long rnd(unsigned *state, long a, long b) {
  if (b <= a)
    return a;
  return a + (long)(rand_r(state) % (unsigned)(b - a + 1));
}

static int read_line(char *buf, size_t size) {
  size_t i = 0;
  flush_out();
  while (i + 1 < size) {
    char c;
    ssize_t r = read(STDIN_FILENO, &c, 1);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      if (i == 0)
        return 0;
      break;
    }
    if (c == '\n')
      break;
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
    if (line[0] == '\0')
      return def;
    char *end;
    long v = strtol(line, &end, 10);
    if (*end == '\0' && v >= lo && v <= hi)
      return v;
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
      "  -m K        приёмов пищи у каждого (0 = бесконечно, остановка по "
      "Ctrl+C)\n"
      "  -t MIN:MAX  пауза между приёмами пищи, мс\n"
      "  -k MIN:MAX  длительность приготовления, мс\n"
      "  -s S        общий запас порций (0 = не ограничен)\n"
      "  -r SEED     начальное значение генератора случайных чисел\n"
      "  -l FILE     файл журнала (по умолчанию cannibals.log, -l '' "
      "отключает)\n"
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
  cfg.meals = (int)ask_long("Приёмов пищи у каждого (0 = бесконечно)", 0,
                            1000000, cfg.meals);
  cfg.tmin = ask_long("Мин. пауза между приёмами, мс", 0, 100000, cfg.tmin);
  cfg.tmax = ask_long("Макс. пауза между приёмами, мс", cfg.tmin, 100000,
                      cfg.tmax < cfg.tmin ? cfg.tmin : cfg.tmax);
  cfg.cmin = ask_long("Мин. время приготовления, мс", 0, 100000, cfg.cmin);
  cfg.cmax = ask_long("Макс. время приготовления, мс", cfg.cmin, 100000,
                      cfg.cmax < cfg.cmin ? cfg.cmin : cfg.cmax);
  cfg.supply = ask_long("Общий запас порций (0 = не ограничен)", 0, 1000000000L,
                        cfg.supply);
  cfg.seed = (unsigned)ask_long("Seed", 0, 2147483647L,
                                (long)(cfg.seed % 2147483647u));
}

static int parse_long(const char *s, long *v) {
  char *e;
  errno = 0;
  *v = strtol(s, &e, 10);
  return *s != '\0' && *e == '\0' && errno == 0;
}

static int parse_int(const char *s, int *v) {
  long x;
  if (!parse_long(s, &x) || x < -2000000000L || x > 2000000000L)
    return 0;
  *v = (int)x;
  return 1;
}

static int parse_range(const char *s, long *a, long *b) {
  return sscanf(s, "%ld:%ld", a, b) == 2 && *a >= 0 && *b >= *a;
}

static int validate(void) {
  if (cfg.n < 1 || cfg.n > MAX_SAVAGES) {
    out("Ошибка: число дикарей должно быть 1..%d\n", MAX_SAVAGES);
    return 0;
  }
  if (cfg.cap < 1) {
    out("Ошибка: вместимость горшка должна быть >= 1\n");
    return 0;
  }
  if (cfg.meals < 0) {
    out("Ошибка: число приёмов пищи не может быть отрицательным\n");
    return 0;
  }
  if (cfg.tmin < 0 || cfg.tmax < cfg.tmin) {
    out("Ошибка: неверный диапазон пауз между приёмами\n");
    return 0;
  }
  if (cfg.cmin < 0 || cfg.cmax < cfg.cmin) {
    out("Ошибка: неверный диапазон времени приготовления\n");
    return 0;
  }
  if (cfg.supply < 0) {
    out("Ошибка: запас порций не может быть отрицательным\n");
    return 0;
  }
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
    case 'n':
      ok = parse_int(v, &cfg.n);
      break;
    case 'c':
      ok = parse_int(v, &cfg.cap);
      break;
    case 'm':
      ok = parse_int(v, &cfg.meals);
      break;
    case 's':
      ok = parse_long(v, &cfg.supply);
      break;
    case 'r':
      ok = parse_long(v, &x) && x >= 0;
      cfg.seed = (unsigned)x;
      break;
    case 't':
      ok = parse_range(v, &cfg.tmin, &cfg.tmax);
      break;
    case 'k':
      ok = parse_range(v, &cfg.cmin, &cfg.cmax);
      break;
    case 'l':
      snprintf(cfg.log_path, sizeof cfg.log_path, "%s", v);
      break;
    default:
      usage(argv[0]);
      return 1;
    }
    if (!ok) {
      out("Ошибка: неверное значение '%s' для опции -%c\n", v, opt);
      return 1;
    }
  }

  if (want_interactive)
    interactive();
  if (!validate())
    return 1;
  return -1;
}