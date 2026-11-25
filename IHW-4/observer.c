// Клиент-наблюдатель: показывает всё, что происходит в системе.
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
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
    int fd = client_connect(&o, "HELLO OBSERVER", &lr, &done);
    if (fd < 0) {
        log_close();
        return fd == CC_SIGNAL ? 130 : fd == CC_FINISHED ? 0 : 1;
    }
    out("Наблюдатель подключён к %s:%d\n", o.addr, o.port);

    char line[512];
    rc = 0;
    for (;;) {
        int r = lr_wait(&lr, line, sizeof line, -1);
        if (r == W_SIG) { out("Наблюдатель отключается по запросу пользователя\n"); rc = 130; break; }
        if (r == W_EOF) { out("Соединение с сервером потеряно\n"); rc = 1; break; }
        if (!strncmp(line, "EV ", 3)) {
            long ms = atol(line + 3);
            const char *text = strchr(line + 3, ' ');
            out("[%7.3f] %s\n", (double)ms / 1000.0, text ? text + 1 : "");
        } else if (!strncmp(line, "TXT ", 4)) {
            out("%s\n", line + 4);
        } else if (!strncmp(line, "SHUTDOWN", 8)) {
            out("Сервер завершил работу (%s), наблюдатель отключается\n", strstr(line, "stopped") ? "остановлен пользователем" : "обед окончен");
            break;
        }
    }
    close(fd);
    log_close();
    return rc;
}
