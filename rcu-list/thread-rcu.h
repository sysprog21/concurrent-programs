/*
 * thread based RCU: Partitioning reference count to per thread storage
 *
 * Provide the multiple-updater for the rcu_assign_pointer by atomic_exchange.
 */

#pragma once

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Wrap the pthread lock API to follow the Linux kernel style. */

/* lock primitives from pthread and compiler primitives */

#include <pthread.h>

typedef pthread_mutex_t spinlock_t;

#define DEFINE_SPINLOCK(lock) spinlock_t lock = PTHREAD_MUTEX_INITIALIZER
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER

static inline void spin_lock_init(spinlock_t *lock)
{
    int ret = pthread_mutex_init(lock, NULL);
    if (ret != 0) {
        fprintf(stderr, "spin_lock_init:pthread_mutex_init %d\n", ret);
        abort();
    }
}

static inline void spin_lock(spinlock_t *lock)
{
    int ret = pthread_mutex_lock(lock);
    if (ret != 0) {
        fprintf(stderr, "spin_lock:pthread_mutex_lock %d\n", ret);
        abort();
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    int ret = pthread_mutex_unlock(lock);
    if (ret != 0) {
        fprintf(stderr, "spin_unlock:pthread_mutex_unlock %d\n", ret);
        abort();
    }
}

#define ACCESS_ONCE(x) (*(volatile __typeof__(x) *) &(x))
#define READ_ONCE(x)                         \
    ({                                       \
        __typeof__(x) ___x = ACCESS_ONCE(x); \
        ___x;                                \
    })
#define WRITE_ONCE(x, val)      \
    do {                        \
        ACCESS_ONCE(x) = (val); \
    } while (0)

#define barrier() __asm__ __volatile__("" : : : "memory")
#define __allow_unused __attribute__((unused))

#define current_tid() (unsigned int) pthread_self()

#ifdef __CHECKER__
#define __rcu __attribute__((noderef, address_space(__rcu)))
#define rcu_check_sparse(p, space) ((void) (((typeof(*p) space *) p) == p))
#define __force __attribute__((force))
#define rcu_uncheck(p) ((__typeof__(*p) __force *) p)
#define rcu_check(p) ((__typeof__(*p) __force __rcu *) p)
#else
#define __rcu
#define rcu_check_sparse(p, space)
#define __force
#define rcu_uncheck(p) p
#define rcu_check(p) p
#endif /* __CHECKER__ */

/* Avoid false sharing */
#define __rcu_aligned __attribute__((aligned(128)))

struct rcu_node {
    unsigned int tid;
    int rcu_nesting[2];
    struct rcu_node *next;
} __rcu_aligned;

struct rcu_data {
    struct rcu_node *head;
    unsigned int nesting_index;
    spinlock_t lock;
};

#define __rcu_thread_index rcu_data.nesting_index
#define __rcu_thread_nesting(ptr) \
    ptr->rcu_nesting[READ_ONCE(__rcu_thread_index) & 1]
#define rcu_thread_nesting __rcu_thread_nesting(__rcu_per_thread_ptr)

static struct rcu_data rcu_data = {
    .head = NULL,
    .nesting_index = 0,
    .lock = SPINLOCK_INIT,
};
static __thread struct rcu_node *__rcu_per_thread_ptr;

static inline struct rcu_node *__rcu_node_add(unsigned int tid)
{
    struct rcu_node **indirect = &rcu_data.head;
    struct rcu_node *node = malloc(sizeof(struct rcu_node));
    if (!node) {
        fprintf(stderr, "__rcu_node_add: malloc failed\n");
        abort();
    }

    node->tid = tid;
    node->rcu_nesting[0] = node->rcu_nesting[1] = 0;
    node->next = NULL;

    spin_lock(&rcu_data.lock);

    while (*indirect) {
        if ((*indirect)->tid == node->tid) {
            spin_unlock(&rcu_data.lock);
            free(node);
            return NULL;
        }
        indirect = &(*indirect)->next;
    }

    *indirect = node;

    spin_unlock(&rcu_data.lock);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return node;
}

static inline int rcu_init(void)
{
    unsigned int tid = current_tid();

    __rcu_per_thread_ptr = __rcu_node_add(tid);
    return !__rcu_per_thread_ptr ? -ENOMEM : 0;
}

static inline void rcu_clean(void)
{
    spin_lock(&rcu_data.lock);

    for (struct rcu_node *node = rcu_data.head; node;) {
        struct rcu_node *tmp = node->next;
        if (__rcu_thread_nesting(node) & 1)
            usleep(10);
        free(node);
        node = tmp;
    }

    rcu_data.head = NULL;

    spin_unlock(&rcu_data.lock);
}

/* The per-thread reference count will only modified by their owner
 * thread but will read by other threads. So here we use WRITE_ONCE().
 */
static inline void rcu_read_lock(void)
{
    WRITE_ONCE(rcu_thread_nesting, 1);
}

static inline void rcu_read_unlock(void)
{
    WRITE_ONCE(rcu_thread_nesting, 0);
}

static inline void synchronize_rcu(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    spin_lock(&rcu_data.lock);

    /* When the rcu_thread_nesting is odd, the thread is in the read-side
     * critical section. Also, we need to skip the read side when it is in
     * the new grace period.
     */
    for (struct rcu_node *node = rcu_data.head; node; node = node->next) {
        while (READ_ONCE(__rcu_thread_nesting(node)) & 1) {
            barrier();
        }
    }

    /* Going to next grace period */
    __atomic_fetch_add(&__rcu_thread_index, 1, __ATOMIC_RELEASE);

    spin_unlock(&rcu_data.lock);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define rcu_dereference(p)                                                 \
    ({                                                                     \
        __typeof__(*p) *__r_d_p = (__typeof__(*p) __force *) READ_ONCE(p); \
        rcu_check_sparse(p, __rcu);                                        \
        __r_d_p;                                                           \
    })

#define rcu_assign_pointer(p, v)                                               \
    ({                                                                         \
        __typeof__(*p) *__r_a_p =                                              \
            (__typeof__(*p) __force *) __atomic_exchange_n(                    \
                &(p), (__typeof__(*(p)) __force __rcu *) v, __ATOMIC_RELEASE); \
        rcu_check_sparse(p, __rcu);                                            \
        __r_a_p;                                                               \
    })
