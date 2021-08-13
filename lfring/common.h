#pragma once

#define CACHE_LINE 64 /* FIXME: should be configurable */

#define INIT_FUNCTION __attribute__((constructor))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define ALIGNED(x) __attribute__((__aligned__(x)))
#if __STDC_VERSION__ >= 201112L
#define THREAD_LOCAL _Thread_local /* C11 */
#else
#define THREAD_LOCAL __thread /* GNU extension */
#endif

#define ROUNDUP_POW2(x)                                    \
    ({                                                     \
        unsigned long _x = (x);                            \
        _x > 1 ? (1UL << (__SIZEOF_LONG__ * __CHAR_BIT__ - \
                          __builtin_clzl(_x - 1UL)))       \
               : 1;                                        \
    })

#define ROUNDUP(a, b)                          \
    ({                                         \
        __typeof__(a) tmp_a = (a);             \
        __typeof__(b) tmp_b = (b);             \
        ((tmp_a + tmp_b - 1) / tmp_b) * tmp_b; \
    })

#if __SIZEOF_POINTER__ == 4
typedef unsigned long long ptrpair_t; /* assume 64 bits */
#else                                 /* __SIZEOF_POINTER__ == 8 */
typedef __int128 ptrpair_t;
#endif

#include <stdlib.h>

static inline void *osal_alloc(size_t size, size_t alignment)
{
    return alignment > 1 ? aligned_alloc(alignment, ROUNDUP(size, alignment))
                         : malloc(size);
}

static inline void osal_free(void *ptr)
{
    free(ptr);
}
