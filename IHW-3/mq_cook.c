// Независимо запускаемый процесс повара (очереди сообщений).
#include <stdlib.h>
#include "mq_proto.h"

int main(int argc, char **argv) {
    if (argc != 1) {
        out("Использование: %s (параметры берутся у cannibals_mq_host)\n", argv[0]);
        return 1;
    }
    signals_install();
    mqd_t cook_q = q_open(Q_COOK, O_RDONLY);
    mqd_t pot_q = q_open(Q_POT, O_WRONLY);
    out_sem = sem_open_wait(SEM_OUT);
    sem_t *ready_s = sem_open_wait(SEM_READY);
    if (cook_q == (mqd_t)-1 || pot_q == (mqd_t)-1 || out_sem == SEM_FAILED || ready_s == SEM_FAILED) {
        out("Не найдены очереди или семафоры: сначала запустите cannibals_mq_host\n");
        return 1;
    }
    out_lock_hooks(out_lock, out_unlock);
    sem_post(ready_s);

    Msg m;
    if (q_recv(cook_q, &m) < 0 || m.type != M_START) return g_sig ? 130 : 1;
    cfg = m.cfg;
    time_set_base(m.a);
    log_open(0);

    unsigned rng = cfg.seed ^ 0x9e3779b9u;
    while (q_recv(cook_q, &m) == 0 && m.type == M_COOK) {
        long dur = rnd(&rng, cfg.cmin, cfg.cmax);
        ev("Повар: проснулся, начинает приготовление (~%ld мс)", dur);
        sleep_ms(dur);
        if (g_sig) break;
        ev("Повар: приготовление закончено (длилось %ld мс)", dur);
        if (q_send(pot_q, M_FILL, 0, cfg.cap, 0) < 0) break;
    }
    log_close();
    return g_sig ? 130 : 0;
}
