#pragma once

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

/* FIXME: make it configurable */
#define CACHELINE_SIZE 64

/* GNU extension */
typedef unsigned __int128 uint128_t;

typedef struct lf_ref lf_ref_t;
struct lf_ref {
    union {
        struct {
            uint64_t val;
            uint64_t tag;
        };
        uint128_t u128;
    };
};
static_assert(sizeof(lf_ref_t) == sizeof(uint128_t), "");
static_assert(alignof(lf_ref_t) == alignof(uint128_t), "");

#define LF_REF_MAKE(_tag, _val) \
    ({                          \
        lf_ref_t r;             \
        r.val = _val;           \
        r.tag = _tag;           \
        r;                      \
    })
#define LF_REF_NULL      \
    ({                   \
        lf_ref_t r = {}; \
        r;               \
    })
#define LF_REF_IS_NULL(ref) ((ref).u128 == 0)
#define LF_REF_EQUAL(a, b) ((a).u128 == (b).u128)

#define LF_REF_CAS(ptr, last, next) _impl_lf_ref_cas(ptr, last, next)
#define LF_U64_CAS(ptr, last, next) _impl_lf_u64_cas(ptr, last, next)

#define LF_ATOMIC_INC(ptr) __sync_add_and_fetch(ptr, 1);
#define LF_ATOMIC_LOAD_ACQUIRE(ptr)                 \
    ({                                              \
        typeof(*ptr) ret;                           \
        __atomic_load(ptr, &ret, __ATOMIC_ACQUIRE); \
        ret;                                        \
    })
#define LF_BARRIER_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define LF_BARRIER_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)

#define LF_ALIGN_UP(s, a) (((s) + ((a) -1)) & ~((a) -1))
#define LF_IS_POW2(s) ((!!(s)) & (!((s) & ((s) -1))))

#if defined(__x86_64__)
#define LF_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#else
#define LF_PAUSE()
#endif

/* FIXME: Rewrite in C11 Atomics */
static inline bool _impl_lf_ref_cas(lf_ref_t *ptr, lf_ref_t prev, lf_ref_t next)
{
    return __sync_bool_compare_and_swap(&ptr->u128, prev.u128, next.u128);
}

/* FIXME: Rewrite in C11 Atomics */
static inline bool _impl_lf_u64_cas(uint64_t *ptr, uint64_t prev, uint64_t next)
{
    return __sync_bool_compare_and_swap(ptr, prev, next);
}
