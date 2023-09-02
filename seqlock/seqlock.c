#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlock.h"

#define SEQLOCK_WRITER 1U

#if defined(__i386__) || defined(__x86_64__)
#define spin_wait() atomic_thread_fence(memory_order_seq_cst)
#elif defined(__aarch64__)
#define spin_wait() __asm__ __volatile__("isb\n")
#else
#define spin_wait() ((void) 0)
#endif

#if defined(__aarch64__)
#define SEVL() sevl()
static inline void sevl(void)
{
    __asm__ volatile("sevl" : : :);
}
#define WFE() wfe()
static inline int wfe(void)
{
    __asm__ volatile("wfe" : : : "memory");
    return 1;
}
#define LDX(a, b) ldx((a), (b))
static inline uint32_t ldx(const uint8_t *var, int mm)
{
    uint32_t old;
    if (mm == memory_order_acquire)
        __asm volatile("ldaxrb %w0, [%1]" : "=&r"(old) : "r"(var) : "memory");
    else if (mm == memory_order_relaxed)
        __asm volatile("ldxrb %w0, [%1]" : "=&r"(old) : "r"(var) : "memory");
    else
        abort();
    return old;
}
#else /* generic */
#define SEVL() (void) 0
#define WFE() 1
#define LDX(a, b) atomic_load_explicit((a), (b))
#endif

#define UNLIKELY(x) __builtin_expect(!!(x), 0)

void seqlock_init(seqlock_t *sync)
{
    *sync = 0;
}

static inline seqlock_t wait_for_no_writer(const seqlock_t *sync, int mo)
{
    seqlock_t l;
    SEVL(); /* Do SEVL early to avoid excessive loop alignment (NOPs) */
    if (UNLIKELY(((l = atomic_load_explicit(sync, mo)) & SEQLOCK_WRITER) !=
                 0)) {
        while (WFE() && ((l = LDX(sync, mo)) & SEQLOCK_WRITER) != 0)
            spin_wait();
    }
    assert((l & SEQLOCK_WRITER) == 0); /* No writer in progress */
    return l;
}

seqlock_t seqlock_acquire_rd(const seqlock_t *sync)
{
    /* Wait for any present writer to go away */
    /* B: Synchronize with A */
    return wait_for_no_writer(sync, memory_order_acquire);
}

bool seqlock_release_rd(const seqlock_t *sync, seqlock_t prv)
{
    /* Enforce Load/Load order as if synchronizing with a store-release or
     * fence-release in another thread.
     */
    atomic_thread_fence(memory_order_acquire);
    /* Test if sync remains unchanged => success */
    return atomic_load_explicit(sync, memory_order_relaxed) == prv;
}

void seqlock_acquire_wr(seqlock_t *sync)
{
    seqlock_t l;
    do {
        /* Wait for any present writer to go away */
        l = wait_for_no_writer(sync, memory_order_relaxed);
        /* Attempt to increment, setting writer flag */
    } while (
        /* C: Synchronize with A */
        !atomic_compare_exchange_strong_explicit(
            sync, (uint32_t *) &l, l + SEQLOCK_WRITER, memory_order_acquire,
            memory_order_relaxed));
    /* Enforce Store/Store order as if synchronizing with a load-acquire or
     * fence-acquire in another thread.
     */
    atomic_thread_fence(memory_order_release);
}

void seqlock_release_wr(seqlock_t *sync)
{
    seqlock_t cur = *sync;
    if (UNLIKELY(cur & SEQLOCK_WRITER) == 0) {
        perror("seqlock: invalid write release");
        return;
    }

    /* Increment, clearing writer flag */
    /* A: Synchronize with B and C */
    atomic_store_explicit(sync, cur + SEQLOCK_WRITER, memory_order_release);
}

#define ATOMIC_COPY(_d, _s, _sz, _type)                                     \
    do {                                                                    \
        const _Atomic _type *src_atomic = (_Atomic const _type *) (_s);     \
        _type val = atomic_load_explicit(src_atomic, memory_order_relaxed); \
        _s += sizeof(_type);                                                \
        _Atomic _type *dst_atomic = (_Atomic _type *) (_d);                 \
        atomic_store_explicit(dst_atomic, val, memory_order_relaxed);       \
        _d += sizeof(_type);                                                \
        _sz -= sizeof(_type);                                               \
    } while (0)

static inline void atomic_memcpy(char *dst, const char *src, size_t sz)
{
#if __SIZEOF_POINTER__ == 8
    while (sz >= sizeof(uint64_t))
        ATOMIC_COPY(dst, src, sz, uint64_t);
    if (sz >= sizeof(uint32_t))
        ATOMIC_COPY(dst, src, sz, uint32_t);
#else  //__SIZEOF_POINTER__ == 4
    while (sz >= sizeof(uint32_t))
        ATOMIC_COPY(dst, src, sz, uint32_t);
#endif
    if (sz >= sizeof(uint16_t))
        ATOMIC_COPY(dst, src, sz, uint16_t);
    if (sz >= sizeof(uint8_t))
        ATOMIC_COPY(dst, src, sz, uint8_t);
}

void seqlock_read(seqlock_t *sync, void *dst, const void *data, size_t len)
{
    seqlock_t prv;
    do {
        prv = seqlock_acquire_rd(sync);
        atomic_memcpy(dst, data, len);
    } while (!seqlock_release_rd(sync, prv));
}

void seqlock_write(seqlock_t *sync, const void *src, void *data, size_t len)
{
    seqlock_acquire_wr(sync);
    atomic_memcpy(data, src, len);
    seqlock_release_wr(sync);
}
