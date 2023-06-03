#pragma once

#include <stdint.h>
#include <time.h>

static inline int64_t wallclock(void)
{
    struct timespec ts[1] = {{}};
    clock_gettime(CLOCK_REALTIME, ts);
    return (int64_t) ts->tv_sec * 1000000000ul + (int64_t) ts->tv_nsec;
}

#define FAIL(...)                     \
    do {                              \
        fprintf(stderr, "FAIL: ");    \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n");        \
        abort();                      \
    } while (0)

#define REQUIRE(expr)                                                         \
    do {                                                                      \
        if (!(expr))                                                          \
            FAIL("REQUIRE FAILED AT %s:%d WITH '%s'", __FUNCTION__, __LINE__, \
                 #expr);                                                      \
    } while (0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
