#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(QUEUE_TYPE) || !defined(QUEUE_MP) || !defined(QUEUE_MC)
#error Please define QUEUE_TYPE, QUEUE_MP and QUEUE_MC
#endif

#if !defined(QUEUE_COMMON_DEFINED)

#define QUEUE_COMMON_DEFINED

#if (__STDC_VERSION__ < 201112L)
#error C11 is required for this file
#endif

#include <stdatomic.h>

#if defined(__STDC_NO_ATOMICS__)
#error Your C compiler does not support C11 atomics
#endif

#define QUEUE_ATOMIC_SIZE_T atomic_size_t
#define QUEUE_ORDER_RELAXED memory_order_relaxed
#define QUEUE_ORDER_RELEASE memory_order_release
#define QUEUE_ORDER_ACQUIRE memory_order_acquire
#define QUEUE_ATOMIC_STORE atomic_store_explicit
#define QUEUE_ATOMIC_LOAD atomic_load_explicit

#define QUEUE_MERGE_BASE(a, b) a##b
#define QUEUE_MERGE(a, b) QUEUE_MERGE_BASE(a, b)

#if !defined(QUEUE_CACHELINE_BYTES)
#define QUEUE_CACHELINE_BYTES 64
#endif

#if !defined(QUEUE_TOO_BIG)
#define QUEUE_TOO_BIG (1024ULL * 1024ULL * 256ULL)
#endif

typedef enum {
    QueueResult_Ok,
    QueueResult_Full,
    QueueResult_Empty,
    QueueResult_Contention,

    QueueResult_Error = 128,
    QueueResult_Error_Too_Small,
    QueueResult_Error_Too_Big,
    QueueResult_Error_Not_Pow2,
    QueueResult_Error_Not_Aligned_16_Bytes,
    QueueResult_Error_Null_Bytes,
    QueueResult_Error_Bytes_Smaller_Than_Needed
} QueueResult_t;
#endif

#if (QUEUE_MP)
#define QUEUE_P_NAME_FN mp
#define QUEUE_P_NAME_TYPE Mp
#define QUEUE_P_TYPE QUEUE_ATOMIC_SIZE_T
#define QUEUE_P_SETUP(a, b, c) QUEUE_ATOMIC_STORE(&a, b, c)
#define QUEUE_P_LOAD(a, b) QUEUE_ATOMIC_LOAD(&a, b)

#define QUEUE_P_IF_CAS(a, b, c, d, e) \
    if (atomic_compare_exchange_weak_explicit(&a, &b, c, d, e))
#else
#define QUEUE_P_NAME_FN sp
#define QUEUE_P_NAME_TYPE Sp
#define QUEUE_P_TYPE size_t
#define QUEUE_P_SETUP(a, b, c)
#define QUEUE_P_LOAD(a, b) a
#define QUEUE_P_IF_CAS(a, b, c, d, e) a = c;
#endif

#if (QUEUE_MC)
#define QUEUE_C_NAME mc
#define QUEUE_C_TYPE QUEUE_ATOMIC_SIZE_T
#define QUEUE_C_SETUP(a, b, c) QUEUE_ATOMIC_STORE(&a, b, c)
#define QUEUE_C_LOAD(a, b) QUEUE_ATOMIC_LOAD(&a, b)

#define QUEUE_C_IF_CAS(a, b, c, d, e) \
    if (atomic_compare_exchange_weak_explicit(&a, &b, c, d, e))
#else
#define QUEUE_C_NAME sc
#define QUEUE_C_TYPE size_t
#define QUEUE_C_SETUP(a, b, c)
#define QUEUE_C_LOAD(a, b) a
#define QUEUE_C_IF_CAS(a, b, c, d, e) a = c;
#endif

#define QUEUE_FN_A QUEUE_MERGE(QUEUE_P_NAME_FN, QUEUE_C_NAME)
#define QUEUE_FN_B QUEUE_MERGE(QUEUE_FN_A, _)
#define QUEUE_FN(name) QUEUE_MERGE(QUEUE_MERGE(QUEUE_FN_B, name##_), QUEUE_TYPE)

#define QUEUE_STRUCT_A QUEUE_MERGE(QUEUE_P_NAME_TYPE, QUEUE_C_NAME)
#define QUEUE_STRUCT_B QUEUE_MERGE(QUEUE_STRUCT_A, _)
#define QUEUE_STRUCT_C QUEUE_MERGE(QUEUE_STRUCT_B, QUEUE_TYPE)
#define QUEUE_STRUCT QUEUE_MERGE(Queue_, QUEUE_STRUCT_C)
#define QUEUE_CELL QUEUE_MERGE(Cell_, QUEUE_STRUCT_C)

typedef struct QUEUE_STRUCT QUEUE_STRUCT;

QueueResult_t QUEUE_FN(make_queue)(size_t cell_count,
                                   QUEUE_STRUCT *queue,
                                   size_t *bytes);

QueueResult_t QUEUE_FN(try_enqueue)(QUEUE_STRUCT *queue,
                                    QUEUE_TYPE const *data);
QueueResult_t QUEUE_FN(try_dequeue)(QUEUE_STRUCT *queue, QUEUE_TYPE *data);
QueueResult_t QUEUE_FN(enqueue)(QUEUE_STRUCT *queue, QUEUE_TYPE const *data);
QueueResult_t QUEUE_FN(dequeue)(QUEUE_STRUCT *queue, QUEUE_TYPE *data);

#if defined(QUEUE_IMPLEMENTATION)
#undef QUEUE_IMPLEMENTATION

typedef struct QUEUE_CELL {
    QUEUE_ATOMIC_SIZE_T sequence;
    QUEUE_TYPE data;
} QUEUE_CELL;

typedef struct QUEUE_STRUCT {
    uint8_t pad0[QUEUE_CACHELINE_BYTES];

    QUEUE_P_TYPE enqueue_index;
    uint8_t pad2[QUEUE_CACHELINE_BYTES - sizeof(QUEUE_P_TYPE)];

    QUEUE_C_TYPE dequeue_index;
    uint8_t pad3[QUEUE_CACHELINE_BYTES - sizeof(QUEUE_C_TYPE)];

    size_t cell_mask;
    uint8_t pad4[QUEUE_CACHELINE_BYTES - sizeof(size_t)];

    QUEUE_CELL cells[];
} QUEUE_STRUCT;

QueueResult_t QUEUE_FN(make_queue)(size_t cell_count,
                                   QUEUE_STRUCT *queue,
                                   size_t *bytes)
{
    if (!bytes)
        return QueueResult_Error_Null_Bytes;

    if (cell_count < 2)
        return QueueResult_Error_Too_Small;

    if (cell_count > QUEUE_TOO_BIG)
        return QueueResult_Error_Too_Big;

    if (cell_count & (cell_count - 1))
        return QueueResult_Error_Not_Pow2;

    size_t bytes_local =
        sizeof(QUEUE_STRUCT) + (sizeof(QUEUE_CELL) * cell_count);

    if (!queue) {
        *bytes = bytes_local;
        return QueueResult_Ok;
    }

    if (*bytes < bytes_local)
        return QueueResult_Error_Bytes_Smaller_Than_Needed;

    {
        intptr_t queue_value = (intptr_t) queue;

        if (queue_value & 0x0F) {
            return QueueResult_Error_Not_Aligned_16_Bytes;
        }
    }

    memset(queue, 0, bytes_local);

    queue->cell_mask = cell_count - 1;

    for (size_t i = 0; i < cell_count; i++) {
        QUEUE_ATOMIC_STORE(&queue->cells[i].sequence, i, QUEUE_ORDER_RELAXED);
    }

    QUEUE_P_SETUP(queue->enqueue_index, 0, QUEUE_ORDER_RELAXED);
    QUEUE_C_SETUP(queue->dequeue_index, 0, QUEUE_ORDER_RELAXED);

    return QueueResult_Ok;
}

QueueResult_t QUEUE_FN(try_enqueue)(QUEUE_STRUCT *queue, QUEUE_TYPE const *data)
{
    size_t pos = QUEUE_P_LOAD(queue->enqueue_index, QUEUE_ORDER_RELAXED);

    QUEUE_CELL *cell = &queue->cells[pos & queue->cell_mask];

    size_t sequence = QUEUE_ATOMIC_LOAD(&cell->sequence, QUEUE_ORDER_ACQUIRE);

    intptr_t difference = (intptr_t) sequence - (intptr_t) pos;

    if (!difference) {
        QUEUE_P_IF_CAS(queue->enqueue_index, pos, pos + 1, QUEUE_ORDER_RELAXED,
                       QUEUE_ORDER_RELAXED)
        {
            cell->data = *data;

            QUEUE_ATOMIC_STORE(&cell->sequence, pos + 1, QUEUE_ORDER_RELEASE);

            return QueueResult_Ok;
        }
    }

    if (difference < 0)
        return QueueResult_Full;

    return QueueResult_Contention;
}

QueueResult_t QUEUE_FN(try_dequeue)(QUEUE_STRUCT *queue, QUEUE_TYPE *data)
{
    size_t pos = QUEUE_C_LOAD(queue->dequeue_index, QUEUE_ORDER_RELAXED);

    QUEUE_CELL *cell = &queue->cells[pos & queue->cell_mask];

    size_t sequence = QUEUE_ATOMIC_LOAD(&cell->sequence, QUEUE_ORDER_ACQUIRE);

    intptr_t difference = (intptr_t) sequence - (intptr_t)(pos + 1);

    if (!difference) {
        QUEUE_C_IF_CAS(queue->dequeue_index, pos, pos + 1, QUEUE_ORDER_RELAXED,
                       QUEUE_ORDER_RELAXED)
        {
            *data = cell->data;

            QUEUE_ATOMIC_STORE(&cell->sequence, pos + queue->cell_mask + 1,
                               QUEUE_ORDER_RELEASE);

            return QueueResult_Ok;
        }
    }

    if (difference < 0) {
        return QueueResult_Empty;
    }

    return QueueResult_Contention;
}

QueueResult_t QUEUE_FN(enqueue)(QUEUE_STRUCT *queue, QUEUE_TYPE const *data)
{
    QueueResult_t result;

    do {
        result = QUEUE_FN(try_enqueue)(queue, data);
    } while (result == QueueResult_Contention);

    return result;
}

QueueResult_t QUEUE_FN(dequeue)(QUEUE_STRUCT *queue, QUEUE_TYPE *data)
{
    QueueResult_t result;

    do {
        result = QUEUE_FN(try_dequeue)(queue, data);
    } while (result == QueueResult_Contention);

    return result;
}
#endif

#undef QUEUE_TYPE
#undef QUEUE_MP
#undef QUEUE_MC

#undef QUEUE_P_NAME_FN
#undef QUEUE_P_NAME_TYPE
#undef QUEUE_P_NAME
#undef QUEUE_P_TYPE
#undef QUEUE_P_SETUP
#undef QUEUE_P_LOAD
#undef QUEUE_P_IF_CAS

#undef QUEUE_C_NAME
#undef QUEUE_C_TYPE
#undef QUEUE_C_SETUP
#undef QUEUE_C_LOAD
#undef QUEUE_C_IF_CAS

#undef QUEUE_FN_A
#undef QUEUE_FN
#undef QUEUE_STRUCT_A
#undef QUEUE_STRUCT_B
#undef QUEUE_STRUCT_C
#undef QUEUE_STRUCT
#undef QUEUE_CELL
