#ifndef NET_H
#define NET_H

#include <stddef.h>

// Протокол: текстовые строки, оканчивающиеся '\n'.
//   клиент -> сервер: HELLO COOK | HELLO SAVAGE <id> | HELLO OBSERVER
//                     TAKE (дикарь), FILL <порций> (повар), BYE
//   сервер -> клиент: WELCOME n cap meals tmin tmax cmin cmax supply seed done server_ms
//                     REJECT <причина>
//                     GRANT <съедено> | STARVED (дикарю), COOK (повару)
//                     EV <мс> <текст> | TXT <текст> (наблюдателю)
//                     SHUTDOWN normal|stopped (всем)

typedef struct {
    int fd;
    char buf[1024];
    int len;
} LineReader;

enum { W_LINE = 1, W_TIMEOUT = 0, W_EOF = -1, W_SIG = -2 };
enum { CC_FAIL = -1, CC_FINISHED = -2, CC_SIGNAL = -3 };

typedef struct {
    char addr[64];
    int port;
    int id;
    int wait_sec;
    char log_path[256];
} ClientOpts;

int  net_listen(const char *addr, int port);
int  net_connect(const char *addr, int port, int wait_sec);
int  net_set_nonblock(int fd);
void net_tune(int fd);
int  net_send(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void lr_init(LineReader *r, int fd);
int  lr_wait(LineReader *r, char *line, size_t cap, long timeout_ms);

int  client_parse(int argc, char **argv, ClientOpts *o, int need_id);
int  client_connect(const ClientOpts *o, const char *hello, LineReader *lr, int *done);
int  client_is_shutdown(const char *line);

#endif
