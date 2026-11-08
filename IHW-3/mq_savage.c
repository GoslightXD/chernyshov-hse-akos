// Независимо запускаемый процесс дикаря (очереди сообщений): cannibals_mq_savage ID
#include <stdlib.h>
#include "mq_proto.h"

int main(int argc, char **argv) {
    char *end = NULL;
    long id = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || *end != '\0' || id < 1 || id > MAX_SAVAGES) {
        out("Использование: %s ID   (номер дикаря, 1..%d)\n", argv[0], MAX_SAVAGES);
        return 1;
    }
    int idx = (int)id - 1;
    signals_install();

    char name[48];
    snprintf(name, sizeof name, Q_REP, idx);
    mqd_t rep_q = q_open(name, O_RDONLY);
    mqd_t pot_q = q_open(Q_POT, O_WRONLY);
    out_sem = sem_open_wait(SEM_OUT);
    sem_t *ready_s = sem_open_wait(SEM_READY);
    if (rep_q == (mqd_t)-1 || pot_q == (mqd_t)-1 || out_sem == SEM_FAILED || ready_s == SEM_FAILED) {
        out("Не найдены очереди или семафоры (дикаря %ld нет в этом запуске?): сначала запустите cannibals_mq_host\n", id);
        return 1;
    }
    out_lock_hooks(out_lock, out_unlock);
    sem_post(ready_s);

    Msg m;
    if (q_recv(rep_q, &m) < 0 || m.type != M_START) return g_sig ? 130 : 1;
    cfg = m.cfg;
    time_set_base(m.a);
    log_open(0);

    unsigned rng = cfg.seed + 7919u * (unsigned)(idx + 1);
    int meals = 0, status = MS_STOPPED;
    for (;;) {
        sleep_ms(rnd(&rng, cfg.tmin, cfg.tmax));
        if (g_sig) break;
        if (q_send(pot_q, M_TAKE, idx, 0, 0) < 0) break;
        if (q_recv(rep_q, &m) < 0) break;
        if (m.type == M_GRANT) {
            meals++;
            if (cfg.meals > 0 && meals >= cfg.meals) {
                ev("Дикарь %d: завершил питание (съел %d порц.)", idx + 1, meals);
                status = MS_DONE;
                break;
            }
        } else if (m.type == M_STARVED) {
            status = MS_STARVED;
            break;
        } else {
            break;      // M_STOP
        }
    }
    q_send(pot_q, M_DONE, idx, status, meals);
    log_close();
    return g_sig ? 130 : 0;
}
