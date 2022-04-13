#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lf_timer.h"

#define CACHE_LINE 64
#define MAXTIMERS 8192

/* Parameters for smp_fence() */
enum {
    LoadLoad = 0x11,
    LoadStore = 0x12,
    StoreLoad = 0x21,
    StoreStore = 0x22,
};

static inline void smp_fence(unsigned int mask)
{
    if ((mask & StoreLoad) == StoreLoad) {
        __asm__ volatile("mfence" ::: "memory");
    } else if (mask != 0) { /* Any fence but StoreLoad */
        __asm__ volatile("" ::: "memory");
    }
}

#include "common.h"
#include "lockfree.h"

struct timer {
    lf_timer_cb cb; /* User-defined callback */
    void *arg;      /* User-defined argument to callback */
};

struct freelist {
    struct timer *head;
    uintptr_t count; /* For ABA protection */
};

static struct {
    lf_tick_t earliest ALIGNED(CACHE_LINE);
    lf_tick_t current;
    uint32_t hi_watermark;

    /* +4 for sentinels */
    lf_tick_t expirations[MAXTIMERS + 4] ALIGNED(CACHE_LINE);
    struct timer timers[MAXTIMERS] ALIGNED(CACHE_LINE);
    struct freelist freelist;
} g_timer;

INIT_FUNCTION
static void init_timers(void)
{
    g_timer.earliest = LF_TIMER_TICK_INVALID;
    g_timer.current = 0;
    g_timer.hi_watermark = 0;
    for (uint32_t i = 0; i < MAXTIMERS; i++) {
        /* All timers beyond hi_watermark <= now */
        g_timer.expirations[i] = 0;
        g_timer.timers[i].cb = NULL;
        g_timer.timers[i].arg = &g_timer.timers[i + 1];
    }

    /* Ensure sentinels trigger expiration compare and loop termination */
    g_timer.expirations[MAXTIMERS + 0] = 0;
    g_timer.expirations[MAXTIMERS + 1] = 0;
    g_timer.expirations[MAXTIMERS + 2] = 0;
    g_timer.expirations[MAXTIMERS + 3] = 0;

    /* Last timer must end freelist */
    g_timer.timers[MAXTIMERS - 1].arg = NULL;

    /* Initialize head of freelist */
    g_timer.freelist.head = g_timer.timers;
    g_timer.freelist.count = 0;
}

/* There might be user-defined data associated with a timer
 * (e.g. accessed through the user-defined argument to the callback)
 * Set (and reset) a timer has release semantics wrt this data
 * Expire a timer thus needs acquire semantics
 */
static void expire_one_timer(lf_tick_t now, lf_tick_t *ptr)
{
    lf_tick_t exp;
    do {
        /* Explicit reloading => smaller code */
        exp = __atomic_load_n(ptr, __ATOMIC_RELAXED);
        if (!(exp <= now)) { /* exp > now */
            /* If timer does not expire anymore it means some thread has
             * (re-)set the timer and then also updated g_timer.earliest
             */
            return;
        }
    } while (!__atomic_compare_exchange_n(ptr, &exp, LF_TIMER_TICK_INVALID,
                                          /* weak = */ true, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED));
    uint32_t tim = ptr - &g_timer.expirations[0];
    g_timer.timers[tim].cb(tim, exp, g_timer.timers[tim].arg);
}

static lf_tick_t scan_timers(lf_tick_t now, lf_tick_t *cur, lf_tick_t *top)
{
    lf_tick_t earliest = LF_TIMER_TICK_INVALID;
    lf_tick_t *ptr = cur;
    lf_tick_t pair0 = *ptr++;
    lf_tick_t pair1 = *ptr++;

    /* Optimize: Interleave loads and compares in order to hide load-to-use
     * latencies. Sentinel will ensure we eventually terminate the loop
     */
    for (;;) {
        lf_tick_t w0 = pair0;
        lf_tick_t w1 = pair1;
        pair0 = *ptr++;
        pair1 = *ptr++;
        if (UNLIKELY(w0 <= now)) {
            lf_tick_t *pw0 = (lf_tick_t *) (ptr - 4);
            if (pw0 >= top)
                break;

            expire_one_timer(now, pw0);
            /* If timer did not actually expire, it was reset by some thread
             * and g_timer.earliest updated which means we do not have to
             * include it in our update of earliest.
             */
        } else { /* 'w0' > 'now' */
            earliest = MIN(earliest, w0);
        }
        if (UNLIKELY(w1 <= now)) {
            lf_tick_t *pw1 = (lf_tick_t *) (ptr - 4) + 1;
            if (pw1 >= top)
                break;
            expire_one_timer(now, pw1);
        } else { /* 'w1' > 'now' */
            earliest = MIN(earliest, w1);
        }
        w0 = pair0;
        w1 = pair1;
        pair0 = *ptr++;
        pair1 = *ptr++;
        if (UNLIKELY(w0 <= now)) {
            lf_tick_t *pw0 = (lf_tick_t *) (ptr - 4);
            if (pw0 >= top)
                break;
            expire_one_timer(now, pw0);
        } else { /* 'w0' > 'now' */
            earliest = MIN(earliest, w0);
        }
        if (UNLIKELY(w1 <= now)) {
            lf_tick_t *pw1 = (lf_tick_t *) (ptr - 4) + 1;
            if (pw1 >= top)
                break;
            expire_one_timer(now, pw1);
        } else { /* 'w1' > 'now' */
            earliest = MIN(earliest, w1);
        }
    }
    return earliest;
}

/* Perform an atomic-min operation on g_timer.earliest */
static inline void update_earliest(lf_tick_t exp)
{
    lf_tick_t old;
    do {
        /* Explicit reloading => smaller code */
        old = __atomic_load_n(&g_timer.earliest, __ATOMIC_RELAXED);
        if (exp >= old) {
            /* Our expiration time is same or later => no update */
            return;
        }
        /* Else our expiration time is earlier than the previous 'earliest' */
    } while (UNLIKELY(!__atomic_compare_exchange_n(
        &g_timer.earliest, &old, exp,
        /* weak = */ true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)));
}

void lf_timer_expire(void)
{
    lf_tick_t now = __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
    lf_tick_t earliest = __atomic_load_n(&g_timer.earliest, __ATOMIC_RELAXED);
    if (earliest <= now) {
        /* There exists at least one timer that is due for expiration */
        PREFETCH_FOR_READ(&g_timer.expirations[0]);
        PREFETCH_FOR_READ((char *) &g_timer.expirations[0] + 1 * CACHE_LINE);
        PREFETCH_FOR_READ((char *) &g_timer.expirations[0] + 2 * CACHE_LINE);
        PREFETCH_FOR_READ((char *) &g_timer.expirations[0] + 3 * CACHE_LINE);

        /* Reset 'earliest' */
        __atomic_store_n(&g_timer.earliest, LF_TIMER_TICK_INVALID,
                         __ATOMIC_RELAXED);

        /* We need our g_timer.earliest reset to be visible before we start to
         * scan the timer array
         */
        smp_fence(StoreLoad);

        /* Scan expiration ticks looking for expired timers */
        earliest = scan_timers(now, &g_timer.expirations[0],
                               &g_timer.expirations[g_timer.hi_watermark]);
        update_earliest(earliest);
    }
    /* Else: no timers due for expiration */
}

void lf_timer_tick_set(lf_tick_t tck)
{
    if (tck == LF_TIMER_TICK_INVALID) {
        fprintf(stderr, "invalid tick: %ld\n", tck);
        return;
    }
    lf_tick_t old = __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
    do {
        if (tck <= old) /* Time cannot run backwards */
            return;
    } while (UNLIKELY(!__atomic_compare_exchange_n(
        &g_timer.current, &old, /* Updated on failure */
        tck,
        /* weak = */ true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)));
}

lf_tick_t lf_timer_tick_get(void)
{
    return __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
}

lf_timer_t lf_timer_alloc(lf_timer_cb cb, void *arg)
{
    union {
        struct freelist fl;
        ptrpair_t pp;
    } old, neu;

    do {
        old.fl.count =
            __atomic_load_n(&g_timer.freelist.count, __ATOMIC_ACQUIRE);
        /* count will be read before head, torn read will be detected by CAS */
        old.fl.head = __atomic_load_n(&g_timer.freelist.head, __ATOMIC_ACQUIRE);
        if (UNLIKELY(old.fl.head == NULL)) {
            return LF_TIMER_NULL;
        }
        neu.fl.head =
            old.fl.head->arg; /* Dereferencing old.head => need acquire */
        neu.fl.count = old.fl.count + 1;
    } while (UNLIKELY(!lockfree_compare_exchange_pp(
        (ptrpair_t *) &g_timer.freelist, &old.pp, neu.pp,
        /* weak = */ true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)));

    uint32_t idx = old.fl.head - g_timer.timers;
    g_timer.expirations[idx] = LF_TIMER_TICK_INVALID;
    g_timer.timers[idx].cb = cb;
    g_timer.timers[idx].arg = arg;

    /* Update high watermark of allocated timers */
    lockfree_fetch_umax_4(&g_timer.hi_watermark, idx + 1, __ATOMIC_RELEASE);
    return idx;
}

void lf_timer_free(lf_timer_t idx)
{
    if (UNLIKELY((uint32_t) idx >= g_timer.hi_watermark)) {
        fprintf(stderr, "invalid timer: %d\n", idx);
        return;
    }

    if (__atomic_load_n(&g_timer.expirations[idx], __ATOMIC_ACQUIRE) !=
        LF_TIMER_TICK_INVALID) {
        fprintf(stderr, "cannot free active timer: %d\n", idx);
        return;
    }

    struct timer *tim = &g_timer.timers[idx];
    union {
        struct freelist fl;
        ptrpair_t pp;
    } old, neu;

    do {
        old.fl = g_timer.freelist;
        tim->cb = NULL;
        tim->arg = old.fl.head;
        neu.fl.head = tim;
        neu.fl.count = old.fl.count + 1;
    } while (UNLIKELY(!lockfree_compare_exchange_pp(
        (ptrpair_t *) &g_timer.freelist, &old.pp, neu.pp,
        /* weak = */ true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)));
}

static inline bool update_expiration(lf_timer_t idx,
                                     lf_tick_t exp,
                                     bool active,
                                     int mo)
{
    if (UNLIKELY((uint32_t) idx >= g_timer.hi_watermark)) {
        fprintf(stderr, "invalid timer: %d", idx);
        return false;
    }

    lf_tick_t old;
    do {
        /* Explicit reloading => smaller code */
        old = __atomic_load_n(&g_timer.expirations[idx], __ATOMIC_RELAXED);
        if (active ? old == LF_TIMER_TICK_INVALID :  // Timer inactive/expired
                old != LF_TIMER_TICK_INVALID) {      // Timer already active
            return false;
        }
    } while (UNLIKELY(
        !__atomic_compare_exchange_n(&g_timer.expirations[idx], &old, exp,
                                     /* weak = */ true, mo, __ATOMIC_RELAXED)));
    if (exp != LF_TIMER_TICK_INVALID)
        update_earliest(exp);
    return true;
}

/* Setting a timer has release order (with regards to user-defined data
 * associated with the timer)
 */
bool lf_timer_set(lf_timer_t idx, lf_tick_t exp)
{
    if (UNLIKELY(exp == LF_TIMER_TICK_INVALID)) {
        fprintf(stderr, "invalid expiration time: %ld\n", exp);
        return false;
    }

    return update_expiration(idx, exp, false, __ATOMIC_RELEASE);
}

bool lf_timer_reset(lf_timer_t idx, lf_tick_t exp)
{
    if (UNLIKELY(exp == LF_TIMER_TICK_INVALID)) {
        fprintf(stderr, "invalid expiration time: %ld\n", exp);
        return false;
    }
    return update_expiration(idx, exp, true, __ATOMIC_RELEASE);
}

bool lf_timer_cancel(lf_timer_t idx)
{
    return update_expiration(idx, LF_TIMER_TICK_INVALID, true,
                             __ATOMIC_RELAXED);
}
