#pragma once

#if USE_PTHREADS

#include <pthread.h>

#define mutex_t pthread_mutex_t
#define mutexattr_t pthread_mutexattr_t
#define mutex_init(m, attr) pthread_mutex_init(m, attr)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#define MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define mutex_trylock(m) (!pthread_mutex_trylock(m))
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define mutexattr_setprotocol pthread_mutexattr_setprotocol
#define PRIO_NONE PTHREAD_PRIO_NONE
#define PRIO_INHERIT PTHREAD_PRIO_INHERIT

#else

#include <stdbool.h>
#include "atomic.h"
#include "futex.h"
#include "spinlock.h"

#define gettid() syscall(SYS_gettid)

typedef struct Mutex mutex_t;
struct Mutex {
    atomic int state;
    bool (*trylock)(mutex_t *);
    void (*lock)(mutex_t *);
    void (*unlock)(mutex_t *);
};

typedef struct {
    int protocol;
} mutexattr_t;

enum {
    MUTEX_LOCKED = 1 << 0,
    MUTEX_SLEEPING = 1 << 1,
};

enum {
    PRIO_NONE = 0,
    PRIO_INHERIT,
};

#define MUTEX_INITIALIZER         \
    {                             \
        .state = 0, .protocal = 0 \
    }

#define MUTEX_SPINS 128

static bool mutex_trylock_default(mutex_t *mutex)
{
    int state = load(&mutex->state, relaxed);
    if (state & MUTEX_LOCKED)
        return false;

    state = fetch_or(&mutex->state, MUTEX_LOCKED, relaxed);
    if (state & MUTEX_LOCKED)
        return false;

    thread_fence(&mutex->state, acquire);
    return true;
}

static inline void mutex_lock_default(mutex_t *mutex)
{
    for (int i = 0; i < MUTEX_SPINS; ++i) {
        if (mutex->trylock(mutex))
            return;
        spin_hint();
    }

    int state = exchange(&mutex->state, MUTEX_LOCKED | MUTEX_SLEEPING, relaxed);

    while (state & MUTEX_LOCKED) {
        futex_wait(&mutex->state, MUTEX_LOCKED | MUTEX_SLEEPING);
        state = exchange(&mutex->state, MUTEX_LOCKED | MUTEX_SLEEPING, relaxed);
    }

    thread_fence(&mutex->state, acquire);
}

static inline void mutex_unlock_default(mutex_t *mutex)
{
    int state = exchange(&mutex->state, 0, release);
    if (state & MUTEX_SLEEPING)
        futex_wake(&mutex->state, 1);  // FFFF
}

/* FIXME: The memory model should be considered carefully. */
#define cmpxchg(obj, expect, desired) \
    compare_exchange_strong(obj, expect, desired, relaxed, relaxed)

static bool mutex_trylock_pi(mutex_t *mutex)
{
    /* TODO: We have FUTEX_TRYLOCK_PI which enable for special
     * trylock in kernel, but it should be fine to just try at
     * userspace now. */
    pid_t zero = 0;
    pid_t tid = gettid();

    /* Try to obtain the lock if it is not contended */
    if (cmpxchg(&mutex->state, &zero, tid))
        return true;

    thread_fence(&mutex->state, acquire);
    return false;
}

static inline void mutex_lock_pi(mutex_t *mutex)
{
    for (int i = 0; i < MUTEX_SPINS; ++i) {
        if (mutex->trylock(mutex))
            return;
        spin_hint();
    }

    /* Since timeout is set as NULL, so we block until the lock is obtain. */
    futex_lock_pi(&mutex->state, NULL);

    thread_fence(&mutex->state, acquire);
}

static inline void mutex_unlock_pi(mutex_t *mutex)
{
    pid_t tid = gettid();

    if (cmpxchg(&mutex->state, &tid, 0))
        return;

    futex_unlock_pi(&mutex->state);
}

static inline void mutex_init(mutex_t *mutex, mutexattr_t *mattr)
{
    atomic_init(&mutex->state, 0);

    // default method
    mutex->trylock = mutex_trylock_default;
    mutex->lock = mutex_lock_default;
    mutex->unlock = mutex_unlock_default;

    if (mattr) {
        switch (mattr->protocol) {
        case PRIO_INHERIT:
            mutex->trylock = mutex_trylock_pi;
            mutex->lock = mutex_lock_pi;
            mutex->unlock = mutex_unlock_pi;
            break;
        default:
            break;
        }
    }
}

static inline bool mutex_trylock(mutex_t *mutex)
{
    return mutex->trylock(mutex);
}

static inline void mutex_lock(mutex_t *mutex)
{
    mutex->lock(mutex);
}

static inline void mutex_unlock(mutex_t *mutex)
{
    mutex->unlock(mutex);
}

static inline void mutexattr_setprotocol(mutexattr_t *mattr, int protocol)
{
    mattr->protocol = protocol;
}

static inline void mutex_destroy(mutex_t *mutex)
{
    /* Do nothing now, just for API convention. */
}

#endif
