// Версия на условных переменных: флаг-предикат + своя cond на каждого дикаря.
#define _POSIX_C_SOURCE 200809L
#include "sync.h"
#include <stdlib.h>

static pthread_mutex_t *mx;
static pthread_cond_t *cv;
static int *woken;
static pthread_cond_t cook_cv = PTHREAD_COND_INITIALIZER;
static int cook_req;
static int count;

void sync_init(int n, pthread_mutex_t *m) {
  mx = m;
  count = n;
  cv = malloc((size_t)n * sizeof *cv);
  woken = calloc((size_t)n, sizeof *woken);
  for (int i = 0; i < n; i++)
    pthread_cond_init(&cv[i], NULL);
}

void sync_destroy(void) {
  for (int i = 0; i < count; i++)
    pthread_cond_destroy(&cv[i]);
  pthread_cond_destroy(&cook_cv);
  free(cv);
  free(woken);
}

void sync_wait_waiter(int idx) {
  while (!woken[idx])
    pthread_cond_wait(&cv[idx], mx);
  woken[idx] = 0;
}

void sync_wake_waiter(int idx) {
  woken[idx] = 1;
  pthread_cond_signal(&cv[idx]);
}

void sync_wait_cook(void) {
  while (!cook_req)
    pthread_cond_wait(&cook_cv, mx);
  cook_req = 0;
}

void sync_wake_cook(void) {
  cook_req = 1;
  pthread_cond_signal(&cook_cv);
}

void sync_wake_all(void) {
  for (int i = 0; i < count; i++)
    sync_wake_waiter(i);
  sync_wake_cook();
}