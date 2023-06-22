#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define ATOMIC_SUB atomic_fetch_sub
#define CAS atomic_compare_exchange_strong

/* The 2nd argument is limited to 1 on machines with TAS but not XCHG.
 * On x86 it is an arbitrary value.
 */
#define XCHG atomic_exchange
#define ATOMIC_ADD atomic_fetch_add
#define mb() atomic_thread_fence(memory_order_seq_cst)

/* Memory barriers*/
#define smp_mb() atomic_thread_fence(memory_order_seq_cst)
#define smp_rmb() atomic_thread_fence(memory_order_acquire)
#define smp_wmb() atomic_thread_fence(memory_order_release)