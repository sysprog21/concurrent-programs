#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "seqlock.h"

#define SEQLOCK_WRITER 1U

#if defined(__i386__) || defined(__x86_64__)
#define spin_wait() __builtin_ia32_pause()
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
    if (mm == __ATOMIC_ACQUIRE)
        __asm volatile("ldaxrb %w0, [%1]" : "=&r"(old) : "r"(var) : "memory");
    else if (mm == __ATOMIC_RELAXED)
        __asm volatile("ldxrb %w0, [%1]" : "=&r"(old) : "r"(var) : "memory");
    else
        abort();
    return old;
}
#else /* generic */
#define SEVL() (void) 0
#define WFE() 1
#define LDX(a, b) __atomic_load_n((a), (b))
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
    if (UNLIKELY(((l = __atomic_load_n(sync, mo)) & SEQLOCK_WRITER) != 0)) {
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
    return wait_for_no_writer(sync, __ATOMIC_ACQUIRE);
}

bool seqlock_release_rd(const seqlock_t *sync, seqlock_t prv)
{
    /* Enforce Load/Load order as if synchronizing with a store-release or
     * fence-release in another thread.
     */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    /* Test if sync remains unchanged => success */
    return __atomic_load_n(sync, __ATOMIC_RELAXED) == prv;
}

void seqlock_acquire_wr(seqlock_t *sync)
{
    seqlock_t l;
    do {
        /* Wait for any present writer to go away */
        l = wait_for_no_writer(sync, __ATOMIC_RELAXED);
        /* Attempt to increment, setting writer flag */
    } while (
        /* C: Synchronize with A */
        !__atomic_compare_exchange_n(sync, &l, l + SEQLOCK_WRITER,
                                     /*weak=*/true, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED));
    /* Enforce Store/Store order as if synchronizing with a load-acquire or
     * fence-acquire in another thread.
     */
    __atomic_thread_fence(__ATOMIC_RELEASE);
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
    __atomic_store_n(sync, cur + 1, __ATOMIC_RELEASE);
}

#define ATOMIC_COPY(_d, _s, _sz, _type)                                      \
    ({                                                                       \
        _type val = __atomic_load_n((const _type *) (_s), __ATOMIC_RELAXED); \
        _s += sizeof(_type);                                                 \
        __atomic_store_n((_type *) (_d), val, __ATOMIC_RELAXED);             \
        _d += sizeof(_type);                                                 \
        _sz -= sizeof(_type);                                                \
    })

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
