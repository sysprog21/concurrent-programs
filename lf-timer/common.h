#pragma once

/* Compiler hints */
#define ALWAYS_INLINE __attribute__((always_inline))
#define INIT_FUNCTION __attribute__((constructor))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Hardware hints */
#define PREFETCH_FOR_READ(ptr) __builtin_prefetch((ptr), 0, 3)
#define PREFETCH_FOR_WRITE(ptr) __builtin_prefetch((ptr), 1, 3)

#define ALIGNED(x) __attribute__((__aligned__(x)))

#define MIN(a, b)                      \
    ({                                 \
        __typeof__(a) tmp_a = (a);     \
        __typeof__(b) tmp_b = (b);     \
        tmp_a < tmp_b ? tmp_a : tmp_b; \
    })

#if __SIZEOF_POINTER__ == 4
typedef unsigned long long ptrpair_t; /* assume 64 bits */
#else                                 /* __SIZEOF_POINTER__ == 8 */
typedef __int128 ptrpair_t;
#endif
