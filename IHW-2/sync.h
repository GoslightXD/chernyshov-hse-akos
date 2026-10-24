#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>

// Все функции, кроме init/destroy, вызываются под мьютексом модели.
// wait-функции отпускают его на время ожидания и возвращаются, снова держа его.

void sync_init(int n, pthread_mutex_t *m);
void sync_destroy(void);

void sync_wait_waiter(int idx);
void sync_wake_waiter(int idx);
void sync_wait_cook(void);
void sync_wake_cook(void);
void sync_wake_all(void); // для остановки: будит всех и повара

#endif