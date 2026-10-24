#ifndef COMMON_H
#define COMMON_H

#include <stdatomic.h>

#define MAX_SAVAGES 1000

typedef struct {
  int n;           // число дикарей
  int cap;         // вместимость горшка
  int meals;       // приёмов пищи у каждого (0 = бесконечно)
  long tmin, tmax; // пауза между приёмами пищи, мс
  long cmin, cmax; // длительность приготовления, мс
  long supply;     // общий запас порций (0 = не ограничен)
  unsigned seed;
  char log_path[256];
} Config;

extern Config cfg;
extern atomic_int g_stop; // выставляется по сигналу, читается всеми потоками

// -1 если можно запускать модель, иначе код выхода
int config_init(int argc, char **argv);

void log_open(void);
void log_close(void);
void flush_out(void);
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void time_start(void);
long now_ms(void);
void sleep_ms(long ms);
long rnd(unsigned *state, long a, long b);

int sim_run(void);

#endif