/* time tracer */

#pragma once

#include <time.h>

#define time_diff(start, end)                         \
    (end.tv_nsec - start.tv_nsec < 0                  \
         ? (1000000000 + end.tv_nsec - start.tv_nsec) \
         : (end.tv_nsec - start.tv_nsec))
#define time_check(_FUNC_)                               \
    do {                                                 \
        struct timespec time_start;                      \
        struct timespec time_end;                        \
        double during;                                   \
        clock_gettime(CLOCK_MONOTONIC, &time_start);     \
        _FUNC_;                                          \
        clock_gettime(CLOCK_MONOTONIC, &time_end);       \
        during = time_diff(time_start, time_end);        \
        printf("[tracer] %s: %f ns\n", #_FUNC_, during); \
    } while (0)
#define __time_check(_FUNC_)                         \
    do {                                             \
        struct timespec time_start;                  \
        struct timespec time_end;                    \
        double during;                               \
        clock_gettime(CLOCK_MONOTONIC, &time_start); \
        _FUNC_;                                      \
        clock_gettime(CLOCK_MONOTONIC, &time_end);   \
        during = time_diff(time_start, time_end);    \
        printf("%f\n", #_FUNC_, during);             \
    } while (0)
#define time_check_return(_FUNC_)                    \
    ({                                               \
        struct timespec time_start;                  \
        struct timespec time_end;                    \
        double during;                               \
        clock_gettime(CLOCK_MONOTONIC, &time_start); \
        _FUNC_;                                      \
        clock_gettime(CLOCK_MONOTONIC, &time_end);   \
        during = time_diff(time_start, time_end);    \
        during;                                      \
    })
#define time_check_loop(_FUNC_, times)                    \
    do {                                                  \
        double sum = 0;                                   \
        int i;                                            \
        for (i = 0; i < times; i++)                       \
            sum += time_check_return(_FUNC_);             \
        printf("[tracer] loop %d : %f ns\n", times, sum); \
    } while (0)
#define time_check_loop_return(_FUNC_, times) \
    ({                                        \
        double sum = 0;                       \
        int i;                                \
        for (i = 0; i < times; i++)           \
            sum += time_check_return(_FUNC_); \
        sum;                                  \
    })
