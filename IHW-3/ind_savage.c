// Независимо запускаемый процесс дикаря: cannibals_savage ID
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include "shm.h"

int main(int argc, char **argv) {
    char *end = NULL;
    long id = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || *end != '\0' || id < 1 || id > MAX_SAVAGES) {
        out("Использование: %s ID   (номер дикаря, 1..%d)\n", argv[0], MAX_SAVAGES);
        return 1;
    }
    signals_install();
    if (shm_attach_existing() < 0) return 1;
    if (id > cfg.n) {
        out("Номер дикаря %ld вне диапазона 1..%d\n", id, cfg.n);
        shm_cleanup();
        return 1;
    }
    log_open(0);
    int rc = savage_run((int)id - 1);
    shm_cleanup();
    log_close();
    return rc ? rc : (g_sig ? 130 : 0);
}
