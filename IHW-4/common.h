#ifndef COMMON_H
#define COMMON_H

#include <signal.h>

#define MAX_SAVAGES 100

typedef struct {
    int n;
    int cap;
    int meals;          // приёмов пищи у каждого, 0 = бесконечно
    long tmin, tmax;    // пауза между приёмами пищи, мс
    long cmin, cmax;    // длительность приготовления, мс
    long supply;        // запас порций, 0 = не ограничен
    unsigned seed;
    char log_path[256];
    char addr[64];      // адрес, на котором сервер слушает
    int port;
} Config;

extern Config cfg;
extern volatile sig_atomic_t g_sig;
extern volatile sig_atomic_t g_usr1;

// -1 если можно запускать, иначе код выхода
int  config_init(int argc, char **argv);

void signals_install(void);
void log_open(const char *path);
void log_close(void);
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

long long mono_ns(void);
void time_set_base(long long ns);
long now_ms(void);
void msleep_raw(long ms);
long rnd(unsigned *state, long a, long b);

#endif
