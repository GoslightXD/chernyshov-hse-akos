// Версия на семафорах: у каждого дикаря и у повара свой семафор.
#define _POSIX_C_SOURCE 200809L
#include "sync.h"
#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>

static pthread_mutex_t *mx;
static sem_t *wake;
static sem_t cook;
static int count;

static void sem_wait_retry(sem_t *s) {
  while (sem_wait(s) == -1 && errno == EINTR)
    ;
}

void sync_init(int n, pthread_mutex_t *m) {
  mx = m;
  count = n;
  wake = calloc((size_t)n, sizeof(sem_t));
  for (int i = 0; i < n; i++)
    sem_init(&wake[i], 0, 0);
  sem_init(&cook, 0, 0);
}

void sync_destroy(void) {
  for (int i = 0; i < count; i++)
    sem_destroy(&wake[i]);
  sem_destroy(&cook);
  free(wake);
}

void sync_wait_waiter(int idx) {
  pthread_mutex_unlock(mx);
  sem_wait_retry(&wake[idx]);
  pthread_mutex_lock(mx);
}

void sync_wake_waiter(int idx) { sem_post(&wake[idx]); }

void sync_wait_cook(void) {
  pthread_mutex_unlock(mx);
  sem_wait_retry(&cook);
  pthread_mutex_lock(mx);
}

void sync_wake_cook(void) { sem_post(&cook); }

void sync_wake_all(void) {
  for (int i = 0; i < count; i++)
    sem_post(&wake[i]);
  sem_post(&cook);
}