#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "sync.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { ST_ACTIVE, ST_DONE, ST_STARVED, ST_STOPPED } Status;

typedef struct {
  int id;
  Status st;
  int meals;
  long wait_ms;
  unsigned rng;
  pthread_t th;
} Savage;

// общее состояние, всё под мьютексом m
static struct {
  int pot;
  long supply_left;
  int request_pending;
  int cooking;
  long empties, requests, filled, taken, fills, cook_busy;
  int wq[MAX_SAVAGES]; // очередь ожидающих повара
  int wq_head, wq_len;
} w;

atomic_int g_stop;
static atomic_int finished;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static Savage sav[MAX_SAVAGES];
static unsigned cook_rng;
static pthread_t cook_th, sig_th;
static sigset_t sigs;

static void fatal(const char *msg) {
  out("НАРУШЕН ИНВАРИАНТ: %s\nАварийное завершение.\n", msg);
  flush_out();
  _exit(2);
}

static void check(void) {
  if (w.pot < 0 || w.pot > cfg.cap)
    fatal("число порций вне [0, вместимость]");
  if (w.filled - w.taken != w.pot)
    fatal("баланс порций не сходится");
  if (w.requests > w.empties)
    fatal("запросов больше, чем опустошений");
}

static void enqueue(int idx) {
  w.wq[(w.wq_head + w.wq_len) % MAX_SAVAGES] = idx;
  w.wq_len++;
}

static int wake_first_waiter(void) {
  if (w.wq_len == 0)
    return 0;
  int idx = w.wq[w.wq_head];
  w.wq_head = (w.wq_head + 1) % MAX_SAVAGES;
  w.wq_len--;
  sync_wake_waiter(idx);
  return 1;
}

// вызывается под мьютексом; 0 - дикарю больше есть не нужно
static int savage_eat(Savage *s) {
  int idx = s->id - 1;
  if (atomic_load(&g_stop)) {
    s->st = ST_STOPPED;
    return 0;
  }
  ev("Дикарь %d: хочет есть, пытается взять порцию (в горшке %d/%d)", s->id,
     w.pot, cfg.cap);
  for (;;) {
    if (w.pot > 0) {
      w.pot--;
      w.taken++;
      s->meals++;
      ev("Дикарь %d: получил порцию #%d, в горшке осталось %d", s->id, s->meals,
         w.pot);
      if (w.pot == 0) {
        w.empties++;
        wake_first_waiter(); // он обнаружит пустой горшок
      }
      check();
      if (cfg.meals > 0 && s->meals >= cfg.meals) {
        s->st = ST_DONE;
        ev("Дикарь %d: завершил питание (съел %d порц.)", s->id, s->meals);
        return 0;
      }
      return 1;
    }
    ev("Дикарь %d: обнаружил пустой горшок", s->id);
    if (cfg.supply > 0 && w.supply_left == 0) {
      s->st = ST_STARVED;
      ev("Дикарь %d: запас исчерпан, уходит голодным (съел %d порц.)", s->id,
         s->meals);
      wake_first_waiter(); // цепочка: следующий ожидающий тоже узнает
      return 0;
    }
    if (!w.request_pending) {
      w.request_pending = 1;
      w.requests++;
      ev("Дикарь %d: будит повара", s->id);
      sync_wake_cook();
    } else {
      ev("Дикарь %d: повар уже разбужен, ждёт без повторного запроса", s->id);
    }
    enqueue(idx);
    long t0 = now_ms();
    sync_wait_waiter(idx);
    s->wait_ms += now_ms() - t0;
    if (atomic_load(&g_stop)) {
      s->st = ST_STOPPED;
      return 0;
    }
  }
}

static void *savage_main(void *arg) {
  Savage *s = arg;
  for (;;) {
    sleep_ms(rnd(&s->rng, cfg.tmin, cfg.tmax));
    pthread_mutex_lock(&m);
    int more = savage_eat(s);
    pthread_mutex_unlock(&m);
    if (!more)
      break;
  }
  return NULL;
}

static void *cook_main(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  for (;;) {
    sync_wait_cook();
    if (atomic_load(&g_stop) || atomic_load(&finished))
      break;
    long start = now_ms();
    long dur = rnd(&cook_rng, cfg.cmin, cfg.cmax);
    w.cooking = 1;
    ev("Повар: проснулся, начинает приготовление (~%ld мс)", dur);
    pthread_mutex_unlock(&m);
    sleep_ms(dur);
    pthread_mutex_lock(&m);
    if (atomic_load(&g_stop))
      break;

    long took = now_ms() - start;
    ev("Повар: приготовление закончено (длилось %ld мс)", took);
    long portions = cfg.cap;
    if (cfg.supply > 0 && portions > w.supply_left)
      portions = w.supply_left;
    if (w.pot != 0)
      fatal("повар пытается наполнить непустой горшок");
    w.pot = (int)portions;
    w.filled += portions;
    if (cfg.supply > 0)
      w.supply_left -= portions;
    w.fills++;
    w.cook_busy += took;
    w.request_pending = 0;
    w.cooking = 0;
    if (cfg.supply > 0)
      ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d, остаток "
         "запаса %ld)",
         portions, w.pot, cfg.cap, w.supply_left);
    else
      ev("Повар: наполнил горшок, положил %ld порц. (в горшке %d/%d)", portions,
         w.pot, cfg.cap);
    ev("Повар: засыпает");
    // будим столько ожидающих, сколько порций; остальные ждут дальше
    for (long k = 0; k < portions; k++)
      if (!wake_first_waiter())
        break;
    check();
  }
  pthread_mutex_unlock(&m);
  return NULL;
}

static void request_stop(void) {
  atomic_store(&g_stop, 1);
  pthread_mutex_lock(&m);
  sync_wake_all();
  pthread_mutex_unlock(&m);
}

static void print_state(void) {
  pthread_mutex_lock(&m);
  ev("Состояние: в горшке %d/%d, ждут повара %d, повар %s, запросов %ld", w.pot,
     cfg.cap, w.wq_len, w.cooking ? "готовит" : "спит", w.requests);
  pthread_mutex_unlock(&m);
}

// все сигналы заблокированы в потоках и принимаются только здесь
static void *signal_main(void *arg) {
  (void)arg;
  int hits = 0, sig;
  for (;;) {
    if (sigwait(&sigs, &sig) != 0)
      continue;
    if (sig == SIGUSR2) {
      if (atomic_load(&finished))
        break;
      continue;
    }
    if (sig == SIGUSR1) {
      print_state();
      continue;
    }
    if (++hits == 1) {
      ev("Получен %s, останавливаем обед (повторный сигнал - немедленный "
         "выход)",
         sig == SIGINT ? "SIGINT" : "SIGTERM");
      request_stop();
    } else {
      out("Повторный сигнал, немедленный выход\n");
      flush_out();
      _exit(130);
    }
  }
  return NULL;
}

static int spawn(pthread_t *t, void *(*f)(void *), void *a) {
  int e = pthread_create(t, NULL, f, a);
  if (e)
    out("Ошибка pthread_create: %s\n", strerror(e));
  return e == 0;
}

static void print_stats(void) {
  int done = 0, starved = 0, stopped = 0;
  for (int i = 0; i < cfg.n; i++) {
    if (sav[i].st == ST_DONE)
      done++;
    else if (sav[i].st == ST_STARVED)
      starved++;
    else
      stopped++;
  }
  out("\n===== Итог =====\n");
  if (atomic_load(&g_stop))
    out("Причина завершения: прервано пользователем\n");
  else if (starved)
    out("Причина завершения: запас порций исчерпан, ожидающие завершены "
        "корректно\n");
  else
    out("Причина завершения: все дикари выполнили заданное число приёмов "
        "пищи\n");
  out("Время работы: %ld мс\n", now_ms());
  out("Повар: наполнений=%ld, время работы=%ld мс, запросов=%ld\n", w.fills,
      w.cook_busy, w.requests);
  out("Горшок: опустошений=%ld, положено порций=%ld, взято порций=%ld, "
      "осталось=%d\n",
      w.empties, w.filled, w.taken, w.pot);
  if (cfg.supply > 0)
    out("Запас: было %ld, осталось %ld\n", cfg.supply, w.supply_left);
  out("Дикари: наелись=%d, ушли голодными=%d, прерваны=%d\n", done, starved,
      stopped);
  out("  №   порций  ждал, мс   статус\n");
  for (int i = 0; i < cfg.n; i++) {
    const char *st = sav[i].st == ST_DONE      ? "наелся"
                     : sav[i].st == ST_STARVED ? "ушёл голодным"
                                               : "прерван";
    out(" %3d  %6d  %8ld   %s\n", sav[i].id, sav[i].meals, sav[i].wait_ms, st);
  }
}

int sim_run(void) {
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  sigaddset(&sigs, SIGUSR1);
  sigaddset(&sigs, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &sigs, NULL);

  time_start();
  w.supply_left = cfg.supply;
  w.empties = 1; // в начале горшок пуст
  cook_rng = cfg.seed ^ 0x9e3779b9u;
  for (int i = 0; i < cfg.n; i++) {
    sav[i].id = i + 1;
    sav[i].rng = cfg.seed + 7919u * (unsigned)(i + 1);
  }
  sync_init(cfg.n, &m);

  out("Seed=%u (порядок событий между потоками от запуска к запуску может "
      "отличаться)\n",
      cfg.seed);
  if (cfg.meals == 0 && cfg.supply == 0)
    out("Режим без ограничений: остановка по Ctrl+C\n");
  ev("Начало обеда: дикарей=%d, вместимость=%d, приёмов=%d, запас=%s", cfg.n,
     cfg.cap, cfg.meals, cfg.supply > 0 ? "ограничен" : "не ограничен");

  int rc = 0, started = 0, cook_ok = 0;
  if (!spawn(&sig_th, signal_main, NULL))
    return 1;
  cook_ok = spawn(&cook_th, cook_main, NULL);
  if (!cook_ok) {
    rc = 1;
    request_stop();
  }
  for (int i = 0; i < cfg.n && rc == 0; i++) {
    if (!spawn(&sav[i].th, savage_main, &sav[i])) {
      rc = 1;
      request_stop();
      break;
    }
    started++;
  }
  for (int i = started; i < cfg.n; i++)
    sav[i].st = ST_STOPPED;
  for (int i = 0; i < started; i++)
    pthread_join(sav[i].th, NULL);

  pthread_mutex_lock(&m);
  atomic_store(&finished, 1);
  sync_wake_cook();
  pthread_mutex_unlock(&m);
  if (cook_ok)
    pthread_join(cook_th, NULL);
  pthread_kill(sig_th, SIGUSR2);
  pthread_join(sig_th, NULL);

  print_stats();
  sync_destroy();
  if (rc)
    return rc;
  return atomic_load(&g_stop) ? 130 : 0;
}