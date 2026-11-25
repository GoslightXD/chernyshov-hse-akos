// Клиент-дикарь: savage -i ID
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common.h"
#include "net.h"

int main(int argc, char **argv) {
    ClientOpts o;
    int rc = client_parse(argc, argv, &o, 1);
    if (rc >= 0) return rc;

    signals_install();
    log_open(o.log_path);
    char hello[48];
    snprintf(hello, sizeof hello, "HELLO SAVAGE %d", o.id);
    LineReader lr;
    int meals = 0;
    int fd = client_connect(&o, hello, &lr, &meals);
    if (fd < 0) {
        log_close();
        return fd == CC_SIGNAL ? 130 : fd == CC_FINISHED ? 0 : 1;
    }
    ev("Дикарь %d: подключился к %s:%d (съел ранее %d порц.)", o.id, o.addr, o.port, meals);

    unsigned rng = cfg.seed + 7919u * (unsigned)o.id;
    char line[256];
    rc = 0;
    int fin = 0;
    while (!fin) {
        // пауза до следующего приёма пищи, сервер может прервать её сообщением
        int r = lr_wait(&lr, line, sizeof line, rnd(&rng, cfg.tmin, cfg.tmax));
        if (r == W_LINE) {
            if (client_is_shutdown(line)) break;
            continue;
        }
        if (r == W_SIG) { ev("Дикарь %d: прервано пользователем", o.id); rc = 130; break; }
        if (r == W_EOF) { ev("Дикарь %d: соединение с сервером потеряно", o.id); rc = 1; break; }

        net_send(fd, "TAKE\n");
        ev("Дикарь %d: хочет есть, отправил запрос горшку", o.id);
        for (;;) {
            r = lr_wait(&lr, line, sizeof line, -1);
            if (r == W_SIG) { ev("Дикарь %d: прервано пользователем", o.id); rc = 130; fin = 1; break; }
            if (r == W_EOF) { ev("Дикарь %d: соединение с сервером потеряно", o.id); rc = 1; fin = 1; break; }
            if (!strncmp(line, "GRANT ", 6)) {
                meals = atoi(line + 6);
                ev("Дикарь %d: получил порцию #%d", o.id, meals);
                if (cfg.meals > 0 && meals >= cfg.meals) {
                    ev("Дикарь %d: завершил питание", o.id);
                    fin = 1;
                }
                break;
            }
            if (!strcmp(line, "STARVED")) {
                ev("Дикарь %d: запас исчерпан, уходит голодным", o.id);
                fin = 1;
                break;
            }
            if (client_is_shutdown(line)) { fin = 1; break; }
        }
    }
    close(fd);
    log_close();
    return rc;
}
