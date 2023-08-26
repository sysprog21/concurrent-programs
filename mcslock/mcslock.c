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

static inline void wait_until_equal_u8(uint8_t *loc, uint8_t val, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
        spin_wait();
}

void mcslock_init(mcslock_t *lock)
{
    *lock = NULL;
}

void mcslock_acquire(mcslock_t *lock, mcsnode_t *node)
{
    node->next = NULL;
    /* A0: Read and write lock, synchronized with A0/A1 */
    mcsnode_t *prev = __atomic_exchange_n(lock, node, __ATOMIC_ACQ_REL);
    if (LIKELY(!prev)) /* Lock uncontended, the lock is acquired */
        return;
    /* Otherwise, the lock is owned by another thread, waiting for its turn */

    node->wait = MCS_WAIT;
    /* B0: Write next, synchronized with B1/B2 */
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);

    /* Waiting for the previous thread to signal using the assigned node
     * C0: Read wait, synchronized with C1
     */
    wait_until_equal_u8(&node->wait, MCS_PROCEED, __ATOMIC_ACQUIRE);
}

void mcslock_release(mcslock_t *lock, mcsnode_t *node)
{
    mcsnode_t *next;

    /* Check if any waiting thread exists */
    /* B1: Read next, synchronized with B0 */
    if ((next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == NULL) {
        /* No waiting threads detected, attempt lock release */
        /* Use temporary variable as it might be overwritten */
        mcsnode_t *tmp = node;

        /* A1: write lock, synchronize with A0 */
        if (__atomic_compare_exchange_n(lock, &tmp, NULL, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            /* No waiting threads yet, lock released successfully */
            return;
        }
        /* Otherwise, at least one waiting thread exists */

        /* Wait for the first waiting thread to link its node with ours */
        /* B2: Read next, synchronized with B0 */
        while ((next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == NULL)
            spin_wait();
    }

    /* Signal the first waiting thread */
    /* C1: Write wait, synchronized with C0 */
    __atomic_store_n(&next->wait, MCS_PROCEED, __ATOMIC_RELEASE);
}
