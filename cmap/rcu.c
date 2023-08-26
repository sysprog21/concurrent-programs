#include <stdatomic.h>
#include <stdlib.h>

#include "list.h"
#include "locks.h"
#include "rcu.h"
#include "util.h"

struct rcu_cb {
    struct list node; /* Inside "struct rcu" */
    rcu_callback_t cb;
    void *args;
};

static inline struct rcu *rcu_allocate_new(void *val)
{
    struct rcu *new_rcu = xmalloc(sizeof(*new_rcu));
    list_init(&new_rcu->cb_list);
    spinlock_init(&new_rcu->lock);
    atomic_init(&new_rcu->counter, 1);
    new_rcu->ptr = val;
    return new_rcu;
}

static void rcu_free(struct rcu *rcu)
{
    struct rcu_cb *rcu_cb;
    LIST_FOREACH_POP (rcu_cb, node, &rcu->cb_list) {
        rcu_cb->cb(rcu_cb->args);
        free(rcu_cb);
    }
    spinlock_destroy(&rcu->lock);
    free(rcu);
}

void rcu_init__(struct rcu **rcu_p, void *val)
{
    struct rcu *new_rcu = rcu_allocate_new(val);
    atomic_init(rcu_p, new_rcu);
}

void rcu_destroy__(struct rcu *rcu)
{
    uint32_t counter = atomic_fetch_sub(&rcu->counter, 1);
    ASSERT(counter == 1);
    rcu_free(rcu);
}

struct rcu *rcu_acquire__(struct rcu **rcu_p)
{
    struct rcu *rcu = atomic_load(rcu_p);
    atomic_fetch_add(&rcu->counter, 1);
    return rcu;
}

void rcu_release__(struct rcu *rcu)
{
    uint32_t counter = atomic_fetch_sub(&rcu->counter, 1);
    if (counter == 1)
        rcu_free(rcu);
}

void rcu_set__(struct rcu **rcu_p, void *val)
{
    struct rcu *old_rcu = atomic_load(rcu_p);
    struct rcu *new_rcu = rcu_allocate_new(val);
    atomic_store(rcu_p, new_rcu);
    uint32_t counter = atomic_fetch_sub(&old_rcu->counter, 1);
    if (counter == 1)
        rcu_free(old_rcu);
}

void rcu_postpone__(struct rcu *rcu,
                    rcu_callback_t cb,
                    void *args,
                    const char *where)
{
    struct rcu_cb *rcu_cb = xmalloc(sizeof(*rcu_cb));
    rcu_cb->cb = cb;
    rcu_cb->args = args;
    spinlock_lock_at(&rcu->lock, where);
    list_push_back(&rcu->cb_list, &rcu_cb->node);
    spinlock_unlock(&rcu->lock);
}
