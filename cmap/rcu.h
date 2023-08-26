#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include "list.h"
#include "locks.h"
#include "util.h"

/* Callback method for RCU type */
typedef void (*rcu_callback_t)(void *);

struct rcu {
    struct list cb_list;  /* Holds "struct rcu_cb" */
    struct spinlock lock; /* Locks on "cb_list" */
    void *ptr;            /* Pointer to data */
    atomic_uint counter;  /* Number of active pointers to this */
};

/* Initiate VAR to VAL */
#define rcu_init(VAR, VAL) rcu_init__(CONST_CAST(struct rcu **, &VAR), VAL)
#define rcu_destroy(VAR) rcu_destroy__(VAR)

/* Acquire & release an RCU pointer
 * Usage:
 * struct rcu* var = rcu_acquire(&rcu);
 * ...
 * rcu_release(var);
 */
#define rcu_acquire(VAR) rcu_acquire__(CONST_CAST(struct rcu **, &VAR))
#define rcu_release(VAR) rcu_release__(VAR)

/* Getter, setter. */
#define rcu_get(VAR, TYPE) ((TYPE) (VAR->ptr))
#define rcu_set(VAR, VAL) rcu_set__(CONST_CAST(struct rcu **, &VAR), VAL)

/* Postpone FUNCTION(ARG) when the current value of VAR is not longer used */
#define rcu_postpone(VAR, FUNCTION, ARG) \
    rcu_postpone__(VAR, FUNCTION, ARG, SOURCE_LOCATOR)

void rcu_init__(struct rcu **, void *val);
void rcu_destroy__(struct rcu *);
struct rcu *rcu_acquire__(struct rcu **);
void rcu_release__(struct rcu *);
void rcu_set__(struct rcu **, void *val);
void rcu_set_and_wait__(struct rcu **, void *val);
void rcu_postpone__(struct rcu *,
                    rcu_callback_t,
                    void *args,
                    const char *where);
