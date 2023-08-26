#pragma once

#include <stdint.h>
#include <time.h>

#define TIMESPAN_GET_NS(DEST, START, END)          \
    DEST = -(START.tv_sec * 1e9 + START.tv_nsec) + \
           (END.tv_sec * 1e9 + END.tv_nsec);

#define TIMESPAN_MEASURE(NAME) clock_gettime(CLOCK_MONOTONIC, &NAME);

/* Start measuring time (nanosec) to variable "name" */
#define PERF_START(name)                      \
    struct timespec name##_start, name##_end; \
    double name;                              \
    TIMESPAN_MEASURE(name##_start);

/* Stop measuring time, store results to variable double "name" */
#define PERF_END(name)            \
    TIMESPAN_MEASURE(name##_end); \
    TIMESPAN_GET_NS(name, name##_start, name##_end)

/* Returns the current time in ms */
static inline uint64_t get_time_ns()
{
    struct timespec clk;
    TIMESPAN_MEASURE(clk);
    return clk.tv_sec * 1e9 + clk.tv_nsec;
}
