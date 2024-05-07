#pragma once

#if USE_PTHREADS

#include <pthread.h>

#define cond_t pthread_cond_t
#define cond_init(c) pthread_cond_init(c, NULL)
#define COND_INITIALIZER PTHREAD_COND_INITIALIZER
#define cond_wait(c, m) pthread_cond_wait(c, m)
#define cond_signal(c, m) pthread_cond_signal(c)
#define cond_broadcast(c, m) pthread_cond_broadcast(c)

#else

#include <limits.h>
#include <stddef.h>
#include "atomic.h"
#include "futex.h"
#include "mutex.h"
#include "spinlock.h"

typedef struct {
    atomic int seq;
} cond_t;

static inline void cond_init(cond_t *cond)
{
    atomic_init(&cond->seq, 0);
}

static inline void cond_wait(cond_t *cond, mutex_t *mutex)
{
    int seq = load(&cond->seq, relaxed);

    mutex_unlock(mutex);

#define COND_SPINS 128
    for (int i = 0; i < COND_SPINS; ++i) {
        if (load(&cond->seq, relaxed) != seq) {
            mutex_lock(mutex);
            return;
        }
        spin_hint();
    }

    futex_wait(&cond->seq, seq);

    mutex_lock(mutex);

    fetch_or(&mutex->state, MUTEX_SLEEPING, relaxed);  // AAAA
}

static inline void cond_signal(cond_t *cond, mutex_t *mutex)
{
    fetch_add(&cond->seq, 1, relaxed);  // BBBB
    futex_wake(&cond->seq, 1);          // EEEE
}

static inline void cond_broadcast(cond_t *cond, mutex_t *mutex)
{
    fetch_add(&cond->seq, 1, relaxed);            // CCCC
    futex_requeue(&cond->seq, 1, &mutex->state);  // DDDD
}

#endif
