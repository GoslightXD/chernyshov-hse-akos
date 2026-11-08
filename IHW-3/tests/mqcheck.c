// Печатает "exists <имя>" для каждой оставшейся очереди проекта.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>

int main(void) {
    char name[48];
    const char *fixed[] = { "/cannibals_pot", "/cannibals_cook" };
    for (int i = 0; i < 2; i++) {
        mqd_t q = mq_open(fixed[i], O_RDONLY);
        if (q != (mqd_t)-1) { printf("exists %s\n", fixed[i]); mq_close(q); }
    }
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "/cannibals_rep_%d", i);
        mqd_t q = mq_open(name, O_RDONLY);
        if (q != (mqd_t)-1) { printf("exists %s\n", name); mq_close(q); }
    }
    return 0;
}
