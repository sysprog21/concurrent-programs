/* Target to use Linux Kernel Memory Model (LKMM) for thread-rcu,
 * C11 memory model might be not compatible with LKMM.
 * Be careful about the architecture or OS you use.
 * You can check the paper to see more detail:
 *
 * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0124r6.html
 */

#ifndef __RCU_H__
#define __RCU_H__

/* lock primitives derived from POSIX Threads and compiler primitives */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef pthread_mutex_t spinlock_t;

#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void spin_lock(spinlock_t *sp)
{
    int ret;

    ret = pthread_mutex_lock(sp);
    if (ret != 0) {
        fprintf(stderr, "spin_lock:pthread_mutex_lock %d\n", ret);
        abort();
    }
}

static inline void spin_unlock(spinlock_t *sp)
{
    int ret;

    ret = pthread_mutex_unlock(sp);
    if (ret != 0) {
        fprintf(stderr, "spin_unlock:pthread_mutex_unlock %d\n", ret);
        abort();
    }
}

#define current_tid() (uintptr_t) pthread_self()

/* Be careful here, since the C11 terms do no have the same sequential
 * consistency for the smp_mb(). Here we use the closely C11 terms,
 * memory_order_seq_cst.
 */
#define smp_mb() atomic_thread_fence(memory_order_seq_cst)

/* Compiler barrier, preventing the compiler from reordering memory accesses */
#define barrier() __asm__ __volatile__("" : : : "memory")

/* To access the shared variable use READ_ONCE() and WRITE_ONCE(). */

/* READ_ONCE() close to those of a C11 volatile memory_order_relaxed atomic
 * read. However, for address, data, or control dependency chain, it is more
 * like memory_order_consume. But, presently most of implementations promote
 * those kind of thing to memory_order_acquire.
 */
#define READ_ONCE(x)                                                      \
    ({                                                                    \
        barrier();                                                        \
        __typeof__(x) ___x = atomic_load_explicit(                        \
            (volatile _Atomic __typeof__(x) *) &x, memory_order_consume); \
        barrier();                                                        \
        ___x;                                                             \
    })

/* WRITE_ONCE() quite close to C11 volatile memory_order_relaxed atomic store */
#define WRITE_ONCE(x, val)                                                  \
    do {                                                                    \
        atomic_store_explicit((volatile _Atomic __typeof__(x) *) &x, (val), \
                              memory_order_relaxed);                        \
    } while (0)

#define smp_store_release(x, val)                                           \
    do {                                                                    \
        atomic_store_explicit((volatile _Atomic __typeof__(x) *) &x, (val), \
                              memory_order_release);                        \
    } while (0)

#define atomic_fetch_add_release(x, v)                       \
    ({                                                       \
        __typeof__(*x) __a_a_r_x;                            \
        atomic_fetch_add_explicit(                           \
            (volatile _Atomic __typeof__(__a_a_r_x) *) x, v, \
            memory_order_release);                           \
    })

#define atomic_fetch_or_release(x, v)                                          \
    ({                                                                         \
        __typeof__(*x) __a_r_r_x;                                              \
        atomic_fetch_or_explicit((volatile _Atomic __typeof__(__a_r_r_x) *) x, \
                                 v, memory_order_release);                     \
    })

#define atomic_xchg_release(x, v)                                              \
    ({                                                                         \
        __typeof__(*x) __a_c_r_x;                                              \
        atomic_exchange_explicit((volatile _Atomic __typeof__(__a_c_r_x) *) x, \
                                 v, memory_order_release);                     \
    })

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __CHECKER__
#define __rcu __attribute__((noderef, address_space(__rcu)))
#define rcu_check_sparse(p, space) ((void) (((typeof(*p) space *) p) == p))
#define __force __attribute__((force))
#define rcu_uncheck(p) ((__typeof__(*(p)) __force *) (p))
#define rcu_check(p) ((__typeof__(*(p)) __force __rcu *) (p))
#else
#define __rcu
#define rcu_check_sparse(p, space)
#define __force
#define rcu_uncheck(p) p
#define rcu_check(p) p
#endif /* __CHECKER__ */

/* Avoid false sharing */
#define CACHE_LINE_SIZE 64
#define __rcu_aligned __attribute__((aligned(2 * CACHE_LINE_SIZE)))

/* Per-thread variable
 *
 * rcu_nesting is to distinguish where the current thread is in which grace
 * period. The reader will use the global variable rcu_nesting_idx to set up
 * the corresponding index of rcu_nesting for the read-side critical section.
 * It is a per-thread variable for optimizing the reader-side lock execution.
 *
 * rcu_nesting uses two lowest bits of __next_rcu_nesting to determine the
 * grace period. Do not use __next_rcu_nesting directly. Use the helper macro
 * to access it.
 */
struct rcu_node {
    unsigned int tid;
    uintptr_t __next_rcu_nesting;
} __rcu_aligned;

struct rcu_data {
    unsigned int nr_thread;
    struct rcu_node *head;
    unsigned int rcu_nesting_idx;
    spinlock_t lock;
};

/* Helper macro of next pointer and rcu_nesting */

#define rcu_nesting(np, idx) \
    (READ_ONCE((np)->__next_rcu_nesting) & (0x1 << ((idx) & (0x1))))
#define rcu_set_nesting(np, idx)                                             \
    do {                                                                     \
        WRITE_ONCE(                                                          \
            (np)->__next_rcu_nesting,                                        \
            READ_ONCE((np)->__next_rcu_nesting) | (0x1 << ((idx) & (0x1)))); \
    } while (0)
#define rcu_unset_nesting(np)                                          \
    do {                                                               \
        smp_store_release((np)->__next_rcu_nesting,                    \
                          READ_ONCE((np)->__next_rcu_nesting) & ~0x3); \
    } while (0)
#define rcu_next(np) \
    ((struct rcu_node *) (READ_ONCE((np)->__next_rcu_nesting) & ~0x3))
#define rcu_next_mask(nrn) ((struct rcu_node *) ((uintptr_t) (nrn) & ~0x3))

static struct rcu_data rcu_data = {
    .nr_thread = 0,
    .head = NULL,
    .rcu_nesting_idx = 0,
    .lock = SPINLOCK_INIT,
};
static __thread struct rcu_node *__rcu_per_thread_ptr;

static inline struct rcu_node *__rcu_node_add(uintptr_t tid)
{
    struct rcu_node **indirect = &rcu_data.head;
    struct rcu_node *node = malloc(sizeof(struct rcu_node));

    if (!node) {
        fprintf(stderr, "__rcu_node_add: malloc failed\n");
        abort();
    }

    node->tid = tid;
    node->__next_rcu_nesting = 0;

    spin_lock(&rcu_data.lock);

    /* Read-side will write the rcu_nesting field in __next_rcu_nesting
     * even if we lock the linked list. So, here we use READ_ONCE().
     */
#define rro_mask(pp) rcu_next_mask(READ_ONCE((*pp)))

    while (rro_mask(indirect)) {
        if (rro_mask(indirect)->tid == node->tid) {
            spin_unlock(&rcu_data.lock);
            free(node);
            return NULL;
        }
        indirect = (struct rcu_node **) &rro_mask(indirect)->__next_rcu_nesting;
    }

#undef rro_mask

    atomic_fetch_or_release((uintptr_t *) indirect, (uintptr_t) node);
    rcu_data.nr_thread++;

    spin_unlock(&rcu_data.lock);

    smp_mb();

    return node;
}

static inline int rcu_init(void)
{
    uintptr_t tid = current_tid();

    __rcu_per_thread_ptr = __rcu_node_add(tid);

    return __rcu_per_thread_ptr ? 0 : -ENOMEM;
}

static inline void rcu_clean(void)
{
    struct rcu_node *node, *tmp;

    spin_lock(&rcu_data.lock);

    for (node = rcu_data.head; node; node = tmp) {
        tmp = rcu_next_mask(node->__next_rcu_nesting);
        free(rcu_next_mask(node));
    }

    rcu_data.head = NULL;
    rcu_data.nr_thread = 0;

    spin_unlock(&rcu_data.lock);
}

/* The per-thread reference count will only modified by their owner
 * thread but will read by other threads. So here we use WRITE_ONCE().
 *
 * We can change the set 1/0 to reference count to make rcu read-side lock
 * nesting. But here we simplified it to become once as time.
 */
static inline void rcu_read_lock(void)
{
    rcu_set_nesting(__rcu_per_thread_ptr, READ_ONCE(rcu_data.rcu_nesting_idx));
}

/* It uses the smp_store_release().
 * But in some case, like MacOS (x86_64, M1), it can use WRITE_ONCE().
 */
static inline void rcu_read_unlock(void)
{
    rcu_unset_nesting(__rcu_per_thread_ptr);
}

static inline void synchronize_rcu(void)
{
    struct rcu_node *node;
    int i;

    smp_mb();

    spin_lock(&rcu_data.lock);

    /* When rcu_nesting is set, the thread is in the read-side critical
     * section. It is safe to plain access rcu_data.rcu_nesting_idx since
     * it only modified by the update-side.
     *
     * Again, if we want the read-side lock to be nesting, we need to change
     * rcu_nesting to reference count.
     */
    for (node = rcu_data.head; node; node = rcu_next(node)) {
        while (rcu_nesting(node, rcu_data.rcu_nesting_idx)) {
            barrier();
        }
    }

    /* Going to next grace period */
    i = atomic_fetch_add_release(&rcu_data.rcu_nesting_idx, 1);

    smp_mb();

    /* Some read-side threads may be in the linked list, but it enters the
     * read-side critical section after the update-side checks it's nesting.
     * It may cause a data race since the update-side will think all the reader
     * passes through the critical section.
     * To stay away from it, we check again after increasing the global index
     * variable.
     */
    for (node = rcu_data.head; node; node = rcu_next(node)) {
        while (rcu_nesting(node, i)) {
            barrier();
        }
    }

    spin_unlock(&rcu_data.lock);

    smp_mb();
}

#define rcu_dereference(p)                                                 \
    ({                                                                     \
        __typeof__(*p) *__r_d_p = (__typeof__(*p) __force *) READ_ONCE(p); \
        rcu_check_sparse(p, __rcu);                                        \
        __r_d_p;                                                           \
    })

#define rcu_assign_pointer(p, v)                                          \
    ({                                                                    \
        rcu_check_sparse(p, __rcu);                                       \
        (__typeof__(*p) __force *) atomic_xchg_release((rcu_uncheck(&p)), \
                                                       rcu_check(v));     \
    })

#endif /* __RCU_H__ */
