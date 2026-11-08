// Хозяин: создаёт shm и семафоры, ждёт независимо запущенных повара и дикарей, печатает итог.
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include "shm.h"

int main(int argc, char **argv) {
    int rc = config_init(argc, argv);
    if (rc >= 0) return rc;

    signals_install();
    log_open(1);
    if (shm_create_new() < 0) return 1;

    out("Seed=%u\n", cfg.seed);
    if (cfg.meals == 0 && cfg.supply == 0) out("Режим без ограничений: остановка по Ctrl+C\n");
    out("Ожидаем независимо запущенные процессы: cannibals_cook и cannibals_savage 1..%d\n", cfg.n);
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
            model_stop();
            deadline = now_ms() + cfg.tmax + cfg.cmax + 2000;   // ждём, пока процессы увидят остановку
        }
        if (g_sig >= 2) {
            forced = 1;
            out("Повторный сигнал, немедленный выход\n");
            break;
        }
        if (stop_sent && now_ms() > deadline) break;
        if (sync_done_wait(100)) done++;
    }
    if (!forced) model_finish();    // отпускаем повара
    print_stats();
    shm_cleanup();
    log_close();
    return (g_sig || forced) ? 130 : 0;
}
