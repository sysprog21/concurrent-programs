#pragma once

#include <stdatomic.h>

#define atomic _Atomic

#define load(obj, order) atomic_load_explicit(obj, memory_order_##order)

#define store(obj, value, order) \
    atomic_store_explicit(obj, value, memory_order_##order)

#define exchange(obj, value, order) \
    atomic_exchange_explicit(obj, value, memory_order_##order)

#define compare_exchange_weak(obj, expected, desired, succ, fail) \
    atomic_compare_exchange_weak_explicit(                        \
        obj, expected, desired, memory_order_##succ, memory_order_##fail)

#define compare_exchange_strong(obj, expected, desired, succ, fail) \
    atomic_compare_exchange_strong_explicit(                        \
        obj, expected, desired, memory_order_##succ, memory_order_##fail)

#define fetch_add(obj, arg, order) \
    atomic_fetch_add_explicit(obj, arg, memory_order_##order)

#define fetch_sub(obj, arg, order) \
    atomic_fetch_sub_explicit(obj, arg, memory_order_##order)

#define fetch_or(obj, arg, order) \
    atomic_fetch_or_explicit(obj, arg, memory_order_##order)

#define fetch_xor(obj, arg, order) \
    atomic_fetch_xor_explicit(obj, arg, memory_order_##order)

#define fetch_and(obj, arg, order) \
    atomic_fetch_and_explicit(obj, arg, memory_order_##order)

/* ThreadSanitizer does not support atomic_thread_fence() */
#ifdef __has_feature
#define TSAN __has_feature(thread_sanitizer)
#else
#define TSAN __SANITIZE_THREAD__
#endif

#if TSAN
#define thread_fence(obj, order) fetch_add(obj, 0, order)
#else
#define thread_fence(obj, order) atomic_thread_fence(memory_order_##order)
#endif
