// Независимо запускаемый процесс повара.
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include "shm.h"

int main(int argc, char **argv) {
    if (argc != 1) {
        out("Использование: %s (параметры берутся у cannibals_host)\n", argv[0]);
        return 1;
    }
    signals_install();
    if (shm_attach_existing() < 0) return 1;
    log_open(0);
    int rc = cook_run();
    shm_cleanup();
    log_close();
    return rc ? rc : (g_sig ? 130 : 0);
}
