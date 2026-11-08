// Родительский процесс запускает повара и дикарей как дочерние процессы (fork).
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "shm.h"

static pid_t pids[MAX_SAVAGES + 1];   // 0..n-1 дикари, n - повар

static pid_t spawn(int idx, int is_cook) {
    pid_t p = fork();
    if (p != 0) return p;
    signal(SIGINT, SIG_IGN);    // Ctrl+C разбирает родитель и останавливает всех через shm
    _exit(is_cook ? cook_run() : savage_run(idx));
}

static void kill_all(void) {
    for (int i = 0; i <= cfg.n; i++)
        if (pids[i] > 0) kill(pids[i], SIGKILL);
}

int main(int argc, char **argv) {
    int rc = config_init(argc, argv);
    if (rc >= 0) return rc;

    signals_install();
    log_open(1);
    if (shm_create_new() < 0) return 1;

    out("Seed=%u\n", cfg.seed);
    if (cfg.meals == 0 && cfg.supply == 0) out("Режим без ограничений: остановка по Ctrl+C\n");
    ev("Начало обеда: дикарей=%d, вместимость=%d, приёмов=%d, запас=%s",
       cfg.n, cfg.cap, cfg.meals, cfg.supply > 0 ? "ограничен" : "не ограничен");

    int alive = 0, bad = 0, forced = 0, stop_sent = 0, finish_sent = 0;
    pids[cfg.n] = spawn(0, 1);
    if (pids[cfg.n] < 0) bad = 1;
    else alive++;
    for (int i = 0; i < cfg.n && !bad; i++) {
        pids[i] = spawn(i, 0);
        if (pids[i] < 0) bad = 1;
        else alive++;
    }
    if (bad) {
        out("Ошибка fork: %s\n", strerror(errno));
        kill_all();
    }
    int savages_left = bad ? 0 : cfg.n;

    while (alive > 0) {
        if (g_usr1) {
            g_usr1 = 0;
            if (!bad && !forced) print_state();
        }
        if (g_sig >= 1 && !stop_sent && !bad) {
            stop_sent = 1;
            ev("Получен сигнал, останавливаем обед (повторный сигнал - немедленный выход)");
            model_stop();
        }
        if (g_sig >= 2 && !forced) {
            forced = 1;
            out("Повторный сигнал, принудительное завершение процессов\n");
            kill_all();
        }
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p == 0) {
            msleep_raw(5);
            continue;
        }
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        alive--;
        int idx = -1;
        for (int i = 0; i <= cfg.n; i++)
            if (pids[i] == p) { idx = i; pids[i] = 0; }
        if (!forced && !bad && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
            bad = 1;
            out("Процесс %d завершился аварийно, останавливаем остальных\n", (int)p);
            kill_all();
        }
        if (idx >= 0 && idx < cfg.n) savages_left--;
        if (savages_left == 0 && !finish_sent && !bad && !forced) {
            finish_sent = 1;
            model_finish();     // отпускаем повара
        }
    }

    print_stats();
    shm_cleanup();
    log_close();
    if (bad) return 1;
    return (g_sig || forced) ? 130 : 0;
}
