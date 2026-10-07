// ИДЗ №1, вариант 6: «Задача о каннибалах».
// Последовательная имитационная программа (один поток, дискретно-событийная
// модель с виртуальным временем).
//
// Сущности модели:
//   - Горшок (World.pot)       : текущее число порций, 0..cap;
//   - Повар (Cook)             : самостоятельный участник, спит, пока его не
//                                разбудит дикарь, нашедший пустой горшок;
//   - Дикари (Savage[])        : самостоятельные участники, периодически
//                                хотят есть и пытаются взять порцию.
//
// Каждый участник – конечный автомат со временем следующего действия.
// Планировщик на каждом шаге выбирает ближайшее по времени событие
// (при равенстве времени: сначала повар, затем дикарь с меньшим номером),
// "перематывает" виртуальное время и выполняет действие участника.
//
// Ввод-вывод выполняется через системные вызовы (open/read/write/close)
// и функции libc, работающие с дескрипторами.
//
// Сборка: gcc -Wall -Wextra -O2 -o cannibals cannibals.c
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAVAGES 1000

// ------------------------------------------------------------------
// Типы данных
// ------------------------------------------------------------------

// Входные параметры задачи
typedef struct {
  int n;              // количество дикарей
  int cap;            // вместимость горшка
  int meals;          // число приёмов пищи каждого дикаря (0 = бесконечно)
  long tmin, tmax;    // диапазон времени между приёмами пищи
  long cmin, cmax;    // диапазон продолжительности приготовления
  long supply;        // общий запас порций (0 = не ограничен)
  unsigned seed;      // начальное значение генератора случайных чисел
  long delay_ms;      // реальная задержка (мс) на одну единицу вирт. времени
  char log_path[256]; // имя файла журнала ("" = журнал не ведётся)
} Config;

typedef enum { S_IDLE, S_WAIT_COOK, S_DONE, S_STARVED } SavageState;

// Дикарь
typedef struct {
  int id;
  SavageState st;
  long next;       // момент следующей попытки поесть (при S_IDLE)
  int meals;       // сколько порций уже получил
  long wait_since; // с какого момента ждёт наполнения горшка
  long total_wait; // суммарное время ожидания повара
} Savage;

typedef enum { C_SLEEP, C_COOKING } CookState;

// Повар
typedef struct {
  CookState st;
  long end;   // момент окончания текущего приготовления
  long start; // момент начала текущего приготовления
  int fills;  // сколько раз наполнил горшок
  long busy;  // суммарное время работы
} Cook;

// Общее состояние мира
typedef struct {
  int pot;             // порций в горшке
  long supply_left;    // остаток запаса (если он ограничен)
  int request_pending; // есть актуальный запрос повару
  long empties;        // сколько раз горшок оказывался пустым
  long requests;       // сколько запросов повару было сделано
  long filled_total;   // всего порций положил повар
  long taken_total;    // всего порций взяли дикари
} World;

static Config cfg;
static World w;
static Cook cook;
static Savage sav[MAX_SAVAGES];
static long now_t = 0;                   // текущее виртуальное время
static int log_fd = -1;                  // дескриптор файла журнала
static volatile sig_atomic_t g_stop = 0; // флаг прерывания по Ctrl+C

// ------------------------------------------------------------------
// Ввод-вывод на системных вызовах
// ------------------------------------------------------------------

// Записать весь буфер в дескриптор (учитывает частичную запись и EINTR).
static void write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t r = write(fd, buf, len);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    buf += r;
    len -= (size_t)r;
  }
}

// Буфер вывода: чтобы не делать по два системных вызова write на каждую
// строку, текст копится и сбрасывается блоками (при заполнении, перед паузой,
// перед чтением ввода и при завершении).
static char obuf[1 << 16];
static size_t olen = 0;

// Сбросить буфер в stdout (fd 1) и в журнал.
static void flush_out(void) {
  if (olen == 0)
    return;
  write_all(STDOUT_FILENO, obuf, olen);
  if (log_fd >= 0)
    write_all(log_fd, obuf, olen);
  olen = 0;
}

// Форматированный вывод одновременно в stdout и в журнал (через буфер).
static void out(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof buf)
    n = (int)sizeof buf - 1;
  if (olen + (size_t)n > sizeof obuf)
    flush_out();
  memcpy(obuf + olen, buf, (size_t)n);
  olen += (size_t)n;
}

// Вывод события с отметкой виртуального времени.
static void ev(const char *fmt, ...) {
  char msg[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  out("[t=%5ld] %s\n", now_t, msg);
}

// Прочитать строку из stdin через read(2). Возвращает 0 при EOF/ошибке.
static int read_line(char *buf, size_t size) {
  size_t i = 0;
  flush_out(); // сначала показать приглашение к вводу
  while (i + 1 < size) {
    char c;
    ssize_t r = read(STDIN_FILENO, &c, 1);
    if (r < 0 && errno == EINTR) {
      if (g_stop)
        return 0;
      continue;
    }
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

// Запрос целого числа у пользователя с проверкой диапазона.
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

// ------------------------------------------------------------------
// Вспомогательные функции
// ------------------------------------------------------------------

// Случайное целое в [a, b].
static long rnd(long a, long b) {
  if (b <= a)
    return a;
  return a + (long)(rand() % (b - a + 1));
}

// Обработчик SIGINT: только выставляет флаг (async-signal-safe).
static void on_sigint(int sig) {
  (void)sig;
  g_stop = 1;
}

// Реальная пауза для наглядности: dt единиц виртуального времени.
static void pause_virtual(long dt) {
  if (cfg.delay_ms <= 0 || dt <= 0)
    return;
  flush_out(); // показать накопленные события до паузы
  long ms = dt * cfg.delay_ms;
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL); // при прерывании сигналом вернётся досрочно
}

// Проверка инвариантов модели после каждого события.
static void check_invariants(void) {
  int ok = 1;
  if (w.pot < 0 || w.pot > cfg.cap) {
    out("НАРУШЕН ИНВАРИАНТ: порций вне [0, cap]\n");
    ok = 0;
  }
  // каждая порция получена не более чем одним дикарём:
  // положено - взято == осталось в горшке
  if (w.filled_total - w.taken_total != w.pot) {
    out("НАРУШЕН ИНВАРИАНТ: баланс порций\n");
    ok = 0;
  }
  long sum = 0;
  for (int i = 0; i < cfg.n; i++)
    sum += sav[i].meals;
  if (sum != w.taken_total) {
    out("НАРУШЕН ИНВАРИАНТ: учёт порций по дикарям\n");
    ok = 0;
  }
  // одно опустошение порождает не более одного запроса
  if (w.requests > w.empties) {
    out("НАРУШЕН ИНВАРИАНТ: запросов больше опустошений\n");
    ok = 0;
  }
  if (!ok) {
    out("Аварийное завершение.\n");
    exit(2);
  }
}

// ------------------------------------------------------------------
// Поведение участников
// ------------------------------------------------------------------

// Разбудить одного ожидающего дикаря – того, кто ждёт дольше всех
// (при равенстве – с меньшим номером). Он повторит попытку в текущий момент.
// Возвращает 1, если кто-то был разбужен.
static int wake_first_waiter(void) {
  int best = -1;
  for (int i = 0; i < cfg.n; i++)
    if (sav[i].st == S_WAIT_COOK &&
        (best < 0 || sav[i].wait_since < sav[best].wait_since))
      best = i;
  if (best < 0)
    return 0;
  sav[best].total_wait += now_t - sav[best].wait_since;
  sav[best].st = S_IDLE;
  sav[best].next = now_t;
  return 1;
}

// Повар закончил приготовление: наполняет горшок и засыпает.
static void cook_finish(void) {
  long portions = cfg.cap;
  ev("Повар: приготовление закончено (длилось %ld)", now_t - cook.start);
  if (cfg.supply > 0 && portions > w.supply_left)
    portions = w.supply_left;
  if (w.pot != 0) { // инвариант: не перезаполнять непустой горшок
    out("НАРУШЕН ИНВАРИАНТ: повар пытается наполнить непустой горшок\n");
    exit(2);
  }
  w.pot = (int)portions;
  w.filled_total += portions;
  if (cfg.supply > 0)
    w.supply_left -= portions;
  cook.fills++;
  cook.busy += now_t - cook.start;
  cook.st = C_SLEEP;
  w.request_pending = 0;
  if (cfg.supply > 0)
    ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d, остаток "
       "запаса %ld)",
       portions, w.pot, cfg.cap, w.supply_left);
  else
    ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d)", portions,
       w.pot, cfg.cap);
  ev("Повар: засыпает");
  // просыпаются столько ожидающих, сколько положено порций (в порядке
  // очереди); остальные продолжают ждать без повторных запросов
  for (long k = 0; k < portions; k++)
    if (!wake_first_waiter())
      break;
}

// Дикарь хочет есть и пытается взять порцию.
static void savage_act(Savage *s) {
  ev("Дикарь %d: хочет есть, пытается взять порцию (в горшке %d/%d)", s->id,
     w.pot, cfg.cap);
  if (w.pot > 0) {
    w.pot--;
    w.taken_total++;
    s->meals++;
    ev("Дикарь %d: получил порцию #%d, в горшке осталось %d", s->id, s->meals,
       w.pot);
    if (w.pot == 0) {
      w.empties++;         // горшок только что опустел
      wake_first_waiter(); // первый из ожидающих обнаружит это и разбудит
                           // повара
    }
    if (cfg.meals > 0 && s->meals >= cfg.meals) {
      s->st = S_DONE;
      ev("Дикарь %d: завершил питание (съел %d порц.)", s->id, s->meals);
    } else {
      s->next = now_t + rnd(cfg.tmin, cfg.tmax);
    }
    return;
  }
  // горшок пуст
  ev("Дикарь %d: обнаружил пустой горшок", s->id);
  if (cfg.supply > 0 && w.supply_left == 0) {
    // повар наполнить уже не сможет – уходит ни с чем
    s->st = S_STARVED;
    ev("Дикарь %d: запас порций исчерпан, уходит голодным (съел %d порц.)",
       s->id, s->meals);
    wake_first_waiter(); // следующий ожидающий тоже должен узнать об этом
    return;
  }
  s->st = S_WAIT_COOK;
  s->wait_since = now_t;
  if (!w.request_pending) {
    w.request_pending = 1;
    w.requests++;
    ev("Дикарь %d: будит повара", s->id);
    cook.st = C_COOKING;
    cook.start = now_t;
    cook.end = now_t + rnd(cfg.cmin, cfg.cmax);
    ev("Повар: проснулся, начинает приготовление (до t=%ld)", cook.end);
  } else {
    ev("Дикарь %d: повар уже разбужен, ждёт наполнения без повторного запроса",
       s->id);
  }
}

// ------------------------------------------------------------------
// Основной цикл имитации
// ------------------------------------------------------------------

static void simulate(void) {
  cook.st = C_SLEEP;
  w.pot = 0;     // в начале горшок пуст, повар спит
  w.empties = 1; // начальное пустое состояние – первое опустошение
  w.supply_left = cfg.supply;
  for (int i = 0; i < cfg.n; i++) {
    sav[i].id = i + 1;
    sav[i].st = S_IDLE;
    sav[i].next = rnd(cfg.tmin, cfg.tmax);
  }
  ev("Начало обеда: дикарей=%d, вместимость=%d, приёмов=%d, запас=%s", cfg.n,
     cfg.cap, cfg.meals, cfg.supply > 0 ? "ограничен" : "не ограничен");

  while (!g_stop) {
    long best = 0;
    int who = -2; // -2 нет событий, -1 повар, >=0 дикарь
    if (cook.st == C_COOKING) {
      best = cook.end;
      who = -1;
    }
    for (int i = 0; i < cfg.n; i++)
      if (sav[i].st == S_IDLE && (who == -2 || sav[i].next < best)) {
        best = sav[i].next;
        who = i;
      }
    if (who == -2)
      break; // все завершили – конец
    pause_virtual(best - now_t);
    if (g_stop)
      break;
    now_t = best;
    if (who == -1)
      cook_finish();
    else
      savage_act(&sav[who]);
    check_invariants();
  }
}

// Итоговая статистика (выводится и в журнал).
static void print_stats(void) {
  int starved = 0, done = 0, waiting = 0;
  for (int i = 0; i < cfg.n; i++) {
    if (sav[i].st == S_STARVED)
      starved++;
    else if (sav[i].st == S_DONE)
      done++;
    else if (sav[i].st == S_WAIT_COOK)
      waiting++;
  }
  out("\n===== Итог =====\n");
  if (g_stop)
    out("Причина завершения: прервано пользователем (SIGINT)\n");
  else if (starved)
    out("Причина завершения: запас порций исчерпан, ожидающие завершены "
        "корректно\n");
  else
    out("Причина завершения: все дикари выполнили заданное число приёмов "
        "пищи\n");
  out("Виртуальное время: %ld\n", now_t);
  out("Повар: наполнений=%d, время работы=%ld, запросов=%ld\n", cook.fills,
      cook.busy, w.requests);
  out("Горшок: опустошений=%ld, положено порций=%ld, взято порций=%ld, "
      "осталось=%d\n",
      w.empties, w.filled_total, w.taken_total, w.pot);
  if (cfg.supply > 0)
    out("Запас: было %ld, осталось %ld\n", cfg.supply, w.supply_left);
  out("Дикари: поели полностью=%d, ушли голодными=%d, ждали при "
      "прерывании=%d\n",
      done, starved, waiting);
  out("  №   порций  ожидание   статус\n");
  for (int i = 0; i < cfg.n; i++) {
    const char *st = sav[i].st == S_DONE        ? "наелся"
                     : sav[i].st == S_STARVED   ? "ушёл голодным"
                     : sav[i].st == S_WAIT_COOK ? "ждал повара"
                                                : "ещё ел бы";
    out(" %3d  %6d  %8ld   %s\n", sav[i].id, sav[i].meals, sav[i].total_wait,
        st);
  }
}

// ------------------------------------------------------------------
// Параметры: значения по умолчанию, аргументы, интерактивный ввод
// ------------------------------------------------------------------

static void usage(const char *prog) {
  out("Использование: %s [опции]\n"
      "  -n N        количество дикарей (1..%d)\n"
      "  -c M        вместимость горшка\n"
      "  -m K        число приёмов пищи каждого дикаря (0 = бесконечно, "
      "остановка Ctrl+C)\n"
      "  -t MIN:MAX  диапазон времени между приёмами пищи\n"
      "  -k MIN:MAX  диапазон продолжительности приготовления\n"
      "  -s S        общий запас порций (0 = не ограничен)\n"
      "  -r SEED     начальное значение генератора случайных чисел\n"
      "  -d MS       реальная задержка в мс на единицу виртуального времени (0 "
      "= без задержки)\n"
      "  -l FILE     файл журнала (по умолчанию cannibals.log, -l '' "
      "отключает)\n"
      "  -i          интерактивный ввод параметров (также при запуске без "
      "аргументов)\n"
      "  -h          эта справка\n",
      prog, MAX_SAVAGES);
}

static void defaults(void) {
  cfg.n = 5;
  cfg.cap = 4;
  cfg.meals = 3;
  cfg.tmin = 1;
  cfg.tmax = 6;
  cfg.cmin = 3;
  cfg.cmax = 8;
  cfg.supply = 0;
  cfg.seed = (unsigned)time(NULL);
  cfg.delay_ms = 100;
  strcpy(cfg.log_path, "cannibals.log");
}

static void interactive(void) {
  out("--- Интерактивный ввод параметров (Enter = значение по умолчанию) "
      "---\n");
  cfg.n = (int)ask_long("Количество дикарей", 1, MAX_SAVAGES, cfg.n);
  cfg.cap = (int)ask_long("Вместимость горшка", 1, 1000000, cfg.cap);
  cfg.meals = (int)ask_long("Приёмов пищи у каждого (0 = бесконечно)", 0,
                            1000000, cfg.meals);
  cfg.tmin = ask_long("Мин. время между приёмами пищи", 0, 100000, cfg.tmin);
  cfg.tmax = ask_long("Макс. время между приёмами пищи", cfg.tmin, 100000,
                      cfg.tmax < cfg.tmin ? cfg.tmin : cfg.tmax);
  cfg.cmin = ask_long("Мин. время приготовления", 0, 100000, cfg.cmin);
  cfg.cmax = ask_long("Макс. время приготовления", cfg.cmin, 100000,
                      cfg.cmax < cfg.cmin ? cfg.cmin : cfg.cmax);
  cfg.supply = ask_long("Общий запас порций (0 = не ограничен)", 0, 1000000000L,
                        cfg.supply);
  cfg.seed = (unsigned)ask_long("Seed генератора", 0, 2147483647L,
                                (long)(cfg.seed % 2147483647u));
  cfg.delay_ms = ask_long("Задержка, мс на единицу времени (0 = без задержки)",
                          0, 10000, cfg.delay_ms);
}

// Разбор "MIN:MAX"; возвращает 1 при успехе.
static int parse_range(const char *s, long *a, long *b) {
  return sscanf(s, "%ld:%ld", a, b) == 2 && *a >= 0 && *b >= *a;
}

// Окончательная проверка параметров.
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
    out("Ошибка: неверный диапазон времени между приёмами\n");
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

int main(int argc, char **argv) {
  atexit(flush_out); // сбросить буфер при любом выходе (в т.ч. по ошибке
                     // параметров)
  defaults();
  int want_interactive = (argc == 1);

  // разбор аргументов командной строки (вручную, без getopt)
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
    char *end;
    switch (opt) {
    case 'n':
      cfg.n = (int)strtol(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 'c':
      cfg.cap = (int)strtol(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 'm':
      cfg.meals = (int)strtol(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 's':
      cfg.supply = strtol(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 'r':
      cfg.seed = (unsigned)strtoul(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 'd':
      cfg.delay_ms = strtol(v, &end, 10);
      if (*end)
        goto bad;
      break;
    case 't':
      if (!parse_range(v, &cfg.tmin, &cfg.tmax))
        goto bad;
      break;
    case 'k':
      if (!parse_range(v, &cfg.cmin, &cfg.cmax))
        goto bad;
      break;
    case 'l':
      strncpy(cfg.log_path, v, sizeof cfg.log_path - 1);
      cfg.log_path[sizeof cfg.log_path - 1] = '\0';
      break;
    default:
      usage(argv[0]);
      return 1;
    bad:
      out("Ошибка: неверное значение '%s' для опции -%c\n", v, opt);
      return 1;
    }
  }

  // обработчик Ctrl+C без SA_RESTART, чтобы прерывать read/nanosleep
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);

  if (want_interactive)
    interactive();
  if (g_stop) {
    out("\nПрервано пользователем до начала имитации.\n");
    return 130;
  }
  if (!validate())
    return 1;

  // журнал: open(2) с O_TRUNC, права 0644
  flush_out(); // то, что уже выведено (ошибки параметров), в журнал не попадает
  if (cfg.log_path[0]) {
    log_fd = open(cfg.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
      out("Предупреждение: не удалось открыть журнал '%s', журнал отключён\n",
          cfg.log_path);
  }

  srand(cfg.seed);
  out("Seed=%u (для воспроизведения: -r %u)\n", cfg.seed, cfg.seed);
  if (cfg.meals == 0 && cfg.supply == 0)
    out("Режим без ограничений по времени: остановка по Ctrl+C\n");

  simulate();
  print_stats();
  flush_out();

  if (log_fd >= 0)
    close(log_fd);
  return g_stop ? 130 : 0;
}