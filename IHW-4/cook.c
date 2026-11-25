// Клиент-повар.
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <unistd.h>
#include "common.h"
#include "net.h"

int main(int argc, char **argv) {
    ClientOpts o;
    int rc = client_parse(argc, argv, &o, 0);
    if (rc >= 0) return rc;

    signals_install();
    log_open(o.log_path);
    LineReader lr;
    int done = 0;
    int fd = client_connect(&o, "HELLO COOK", &lr, &done);
    if (fd < 0) {
        log_close();
        return fd == CC_SIGNAL ? 130 : fd == CC_FINISHED ? 0 : 1;
    }
    ev("Повар: подключился к %s:%d", o.addr, o.port);

    unsigned rng = cfg.seed ^ 0x9e3779b9u;
    char line[256];
    rc = 0;
    for (int run = 1; run;) {
        int r = lr_wait(&lr, line, sizeof line, -1);
        if (r == W_SIG) { ev("Прервано пользователем"); rc = 130; break; }
        if (r == W_EOF) { ev("Соединение с сервером потеряно"); rc = 1; break; }
        if (client_is_shutdown(line)) break;
        if (strcmp(line, "COOK") != 0) continue;

        long dur = rnd(&rng, cfg.cmin, cfg.cmax);
        long end = now_ms() + dur;
        ev("Повар: проснулся, начинает приготовление (~%ld мс)", dur);
        for (;;) {
            long left = end - now_ms();
            if (left <= 0) {
                net_send(fd, "FILL %d\n", cfg.cap);
                ev("Повар: приготовление закончено, отправил порции горшку");
                break;
            }
            r = lr_wait(&lr, line, sizeof line, left);
            if (r == W_TIMEOUT) continue;
            if (r == W_SIG) { ev("Прервано пользователем"); rc = 130; run = 0; break; }
            if (r == W_EOF) { ev("Соединение с сервером потеряно"); rc = 1; run = 0; break; }
            if (client_is_shutdown(line)) { run = 0; break; }
        }
    }
    close(fd);
    log_close();
    return rc;
}
