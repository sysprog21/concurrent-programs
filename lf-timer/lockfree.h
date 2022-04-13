#pragma once

#include "common.h"

#define HAS_ACQ(mo) ((mo) != __ATOMIC_RELAXED && (mo) != __ATOMIC_RELEASE)
#define HAS_RLS(mo)                                          \
    ((mo) == __ATOMIC_RELEASE || (mo) == __ATOMIC_ACQ_REL || \
     (mo) == __ATOMIC_SEQ_CST)

#define MO_LOAD(mo) (HAS_ACQ((mo)) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED)
#define MO_STORE(mo) (HAS_RLS((mo)) ? __ATOMIC_RELEASE : __ATOMIC_RELAXED)

#include <stdbool.h>

union u128 {
    struct {
        uint64_t lo, hi;
    } s;
    __int128 ui;
};

ALWAYS_INLINE
static inline bool cmpxchg16b(__int128 *src, union u128 *cmp, union u128 with)
{
    bool result;
    __asm__ __volatile__("lock cmpxchg16b %1\n\tsetz %0"
                         : "=q"(result), "+m"(*src), "+d"(cmp->s.hi),
                           "+a"(cmp->s.lo)
                         : "c"(with.s.hi), "b"(with.s.lo)
                         : "cc", "memory");
    return result;
}

ALWAYS_INLINE
static inline bool lockfree_compare_exchange_16(register __int128 *var,
                                                __int128 *exp,
                                                __int128 neu,
                                                bool weak,
                                                int mo_success,
                                                int mo_failure)
{
    (void) weak;
    (void) mo_success;
    (void) mo_failure;
    union u128 cmp = {.ui = *exp}, with = {.ui = neu};
    bool ret = cmpxchg16b(var, &cmp, with);
    if (UNLIKELY(!ret))
        *exp = cmp.ui;
    return ret;
}

#define lockfree_compare_exchange_pp lockfree_compare_exchange_16

ALWAYS_INLINE
static inline uint32_t lockfree_fetch_umax_4(uint32_t *var,
                                             uint32_t val,
                                             int mo)
{
    uint32_t old = __atomic_load_n(var, __ATOMIC_RELAXED);
    do {
        if (val <= old) {
            return old;
        }
    } while (!__atomic_compare_exchange_n(
        var, &old, val,
        /*weak=*/true, MO_LOAD(mo) | MO_STORE(mo), MO_LOAD(mo)));
    return old;
}
