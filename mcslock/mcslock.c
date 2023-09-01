#include <stdatomic.h>
#include <stddef.h>

#include "mcslock.h"

#define LIKELY(x) __builtin_expect(!!(x), 1)

enum { MCS_PROCEED = 0, MCS_WAIT = 1 };

#if defined(__i386__) || defined(__x86_64__)
#define spin_wait() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define spin_wait() __asm__ __volatile__("isb\n")
#else
#define spin_wait() ((void) 0)
#endif

static inline void wait_until_equal_u8(_Atomic uint8_t *loc,
                                       uint8_t val,
                                       memory_order mm)
{
    while (atomic_load_explicit(loc, mm) != val)
        spin_wait();
}

void mcslock_init(mcslock_t *lock)
{
    atomic_init(lock, NULL);
}

void mcslock_acquire(mcslock_t *lock, mcsnode_t *node)
{
    atomic_init(&node->next, NULL);
    /* A0: Read and write lock, synchronized with A0/A1 */
    mcsnode_t *prev =
        atomic_exchange_explicit(lock, node, memory_order_acq_rel);
    if (LIKELY(!prev)) /* Lock uncontended, the lock is acquired */
        return;
    /* Otherwise, the lock is owned by another thread, waiting for its turn */

    atomic_store_explicit(&node->wait, MCS_WAIT, memory_order_release);
    /* B0: Write next, synchronized with B1/B2 */
    atomic_store_explicit(&prev->next, node, memory_order_release);

    /* Waiting for the previous thread to signal using the assigned node
     * C0: Read wait, synchronized with C1
     */
    wait_until_equal_u8(&node->wait, MCS_PROCEED, memory_order_acquire);
}

void mcslock_release(mcslock_t *lock, mcsnode_t *node)
{
    mcsnode_t *next;

    /* Check if any waiting thread exists */
    /* B1: Read next, synchronized with B0 */
    if ((next = atomic_load_explicit(&node->next, memory_order_acquire)) ==
        NULL) {
        /* No waiting threads detected, attempt lock release */
        /* Use temporary variable as it might be overwritten */
        mcsnode_t *tmp = node;

        /* A1: write lock, synchronize with A0 */
        if (atomic_compare_exchange_strong_explicit(
                lock, &tmp, NULL, memory_order_release, memory_order_relaxed)) {
            /* No waiting threads yet, lock released successfully */
            return;
        }
        /* Otherwise, at least one waiting thread exists */

        /* Wait for the first waiting thread to link its node with ours */
        /* B2: Read next, synchronized with B0 */
        while ((next = atomic_load_explicit(&node->next,
                                            memory_order_acquire)) == NULL)
            spin_wait();
    }

    /* Signal the first waiting thread */
    /* C1: Write wait, synchronized with C0 */
    atomic_store_explicit(&next->wait, MCS_PROCEED, memory_order_release);
}
