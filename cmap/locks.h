#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "util.h"

struct spinlock {
    const char *where;
    atomic_uint value;
};

static inline void spinlock_init(struct spinlock *spin);
static inline void spinlock_destroy(struct spinlock *spin);

/* Locks "spin" and returns status code */
#define spinlock_lock(spin) spinlock_lock_at(spin, SOURCE_LOCATOR)

static inline void spinlock_lock_at(struct spinlock *spin, const char *where);
static inline void spinlock_unlock(struct spinlock *spin);

struct mutex {
    const char *where;
    pthread_mutex_t lock;
};

static inline void mutex_init(struct mutex *mutex);
static inline void mutex_destroy(struct mutex *mutex);

#define mutex_lock(mutex) mutex_lock_at(mutex, SOURCE_LOCATOR);

static inline void mutex_lock_at(struct mutex *mutex_, const char *where);
static inline void mutex_unlock(struct mutex *mutex_);

struct cond {
    pthread_cond_t cond;
    struct mutex mutex;
    atomic_uint value;
};

#define cond_wait(cond) cond_wait_at(cond, SOURCE_LOCATOR);
#define cond_lock(cond) cond_lock_at(cond, SOURCE_LOCATOR);

static inline void cond_init(struct cond *cond);
static inline void cond_destroy(struct cond *cond);
static inline void cond_wait_at(struct cond *cond, const char *where);
static inline void cond_lock_at(struct cond *cond, const char *where);
static inline void cond_unlock(struct cond *cond);
static inline bool cond_is_locked(struct cond *cond);

static inline void spinlock_init(struct spinlock *spin)
{
    if (!spin)
        return;
    atomic_init(&spin->value, 0);
    spin->where = NULL;
}

static inline void spinlock_destroy(struct spinlock *spin)
{
    if (!spin)
        return;
    ASSERT(atomic_load(&spin->value) == 0);
    spin->where = NULL;
}

static inline void spinlock_lock_at(struct spinlock *spin, const char *where)
{
    if (!spin)
        return;
    uint32_t zero = 0;
    while (!atomic_compare_exchange_strong(&spin->value, &zero, 1))
        zero = 0;
    spin->where = where;
}

static inline void spinlock_unlock(struct spinlock *spin)
{
    if (!spin)
        return;
    ASSERT(atomic_load(&spin->value) == 1);
    atomic_store(&spin->value, 0);
    spin->where = NULL;
}

static inline void mutex_init(struct mutex *mutex)
{
    if (pthread_mutex_init(&mutex->lock, NULL))
        abort_msg("pthread_mutex_init fail");
}

static inline void mutex_destroy(struct mutex *mutex)
{
    if (pthread_mutex_destroy(&mutex->lock))
        abort_msg("pthread_mutex_destroy fail");
}

static inline void mutex_lock_at(struct mutex *mutex, const char *where)
{
    if (!mutex)
        return;
    if (pthread_mutex_lock(&mutex->lock))
        abort_msg("pthread_mutex_lock fail");
    atomic_store_explicit(&mutex->where, where, memory_order_relaxed);
}

static inline void mutex_unlock(struct mutex *mutex)
{
    if (!mutex)
        return;
    if (pthread_mutex_unlock(&mutex->lock))
        abort_msg("pthread_mutex_unlock fail");
    atomic_store_explicit(&mutex->where, NULL, memory_order_relaxed);
}

static inline void cond_init(struct cond *cond)
{
    if (pthread_cond_init(&cond->cond, NULL))
        abort_msg("pthread_cond_init fail");
    mutex_init(&cond->mutex);
    atomic_init(&cond->value, 0);
}

static inline void cond_destroy(struct cond *cond)
{
    uint32_t value = atomic_load(&cond->value);
    ASSERT(!value);
    if (pthread_cond_destroy(&cond->cond))
        abort_msg("pthread_cond_destroy fail");
    mutex_destroy(&cond->mutex);
}

static inline void cond_wait_at(struct cond *cond, const char *where)
{
    mutex_lock_at(&cond->mutex, where);
    while (1) {
        uint32_t value = atomic_load(&cond->value);
        if (!value)
            break;
        if (pthread_cond_wait(&cond->cond, &cond->mutex.lock))
            abort_msg("pthread_cond_wait fail");
    }
    mutex_unlock(&cond->mutex);
}

static inline void cond_lock_at(struct cond *cond, const char *where)
{
    atomic_store(&cond->value, 1);
}

static inline bool cond_is_locked(struct cond *cond)
{
    uint32_t value = atomic_load(&cond->value);
    return value == 1;
}

static inline void cond_unlock(struct cond *cond)
{
    mutex_lock(&cond->mutex);
    atomic_store(&cond->value, 0);
    if (pthread_cond_broadcast(&cond->cond))
        abort_msg("pthread_cond_broadcast fail");
    mutex_unlock(&cond->mutex);
}
