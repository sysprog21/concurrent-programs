#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h> /* C11 */

#define QUEUE_TEST_THREADS_MAX 16

typedef struct Data {
    float a;
    uint32_t b;
    uint8_t bytes[16];
} Data;

#define QUEUE_MP 0
#define QUEUE_MC 0
#define QUEUE_TYPE Data
#define QUEUE_IMPLEMENTATION
#include "queues.h"

#define QUEUE_MP 1
#define QUEUE_MC 0
#define QUEUE_TYPE Data
#define QUEUE_IMPLEMENTATION
#include "queues.h"

#define QUEUE_MP 0
#define QUEUE_MC 1
#define QUEUE_TYPE Data
#define QUEUE_IMPLEMENTATION
#include "queues.h"

#define QUEUE_MP 1
#define QUEUE_MC 1
#define QUEUE_TYPE Data
#define QUEUE_IMPLEMENTATION
#include "queues.h"

#define CAST(x, y) ((x) y)

typedef enum Tag {
    Spsc,
    Mpsc,
    Spmc,
    Mpmc,
    Max = Mpmc,
} Tag;

static const char *tag_to_name[] = {"Spsc", "Mpsc", "Spmc", "Mpmc"};

QueueResult_t make(Tag tag, size_t cell_count, void *queue, size_t *bytes)
{
    switch (tag) {
    case Spsc:
        return spsc_make_queue_Data(cell_count, CAST(Queue_Spsc_Data *, queue),
                                    bytes);
    case Mpsc:
        return mpsc_make_queue_Data(cell_count, CAST(Queue_Mpsc_Data *, queue),
                                    bytes);
    case Spmc:
        return spmc_make_queue_Data(cell_count, CAST(Queue_Spmc_Data *, queue),
                                    bytes);
    case Mpmc:
        return mpmc_make_queue_Data(cell_count, CAST(Queue_Mpmc_Data *, queue),
                                    bytes);
    }

    return QueueResult_Error;
}

QueueResult_t try_enqueue(Tag tag, void *q, Data const *d)
{
    switch (tag) {
    case Spsc:
        return spsc_try_enqueue_Data(CAST(Queue_Spsc_Data *, q), d);
    case Mpsc:
        return mpsc_try_enqueue_Data(CAST(Queue_Mpsc_Data *, q), d);
    case Spmc:
        return spmc_try_enqueue_Data(CAST(Queue_Spmc_Data *, q), d);
    case Mpmc:
        return mpmc_try_enqueue_Data(CAST(Queue_Mpmc_Data *, q), d);
    }

    return QueueResult_Error;
}
QueueResult_t try_dequeue(Tag tag, void *q, Data *d)
{
    switch (tag) {
    case Spsc:
        return spsc_try_dequeue_Data(CAST(Queue_Spsc_Data *, q), d);
    case Mpsc:
        return mpsc_try_dequeue_Data(CAST(Queue_Mpsc_Data *, q), d);
    case Spmc:
        return spmc_try_dequeue_Data(CAST(Queue_Spmc_Data *, q), d);
    case Mpmc:
        return mpmc_try_dequeue_Data(CAST(Queue_Mpmc_Data *, q), d);
    }

    return QueueResult_Error;
}
QueueResult_t enqueue(Tag tag, void *q, Data const *d)
{
    switch (tag) {
    case Spsc:
        return spsc_enqueue_Data(CAST(Queue_Spsc_Data *, q), d);
    case Mpsc:
        return mpsc_enqueue_Data(CAST(Queue_Mpsc_Data *, q), d);
    case Spmc:
        return spmc_enqueue_Data(CAST(Queue_Spmc_Data *, q), d);
    case Mpmc:
        return mpmc_enqueue_Data(CAST(Queue_Mpmc_Data *, q), d);
    }

    return QueueResult_Error;
}
QueueResult_t dequeue(Tag tag, void *q, Data *d)
{
    switch (tag) {
    case Spsc:
        return spsc_dequeue_Data(CAST(Queue_Spsc_Data *, q), d);
    case Mpsc:
        return mpsc_dequeue_Data(CAST(Queue_Mpsc_Data *, q), d);
    case Spmc:
        return spmc_dequeue_Data(CAST(Queue_Spmc_Data *, q), d);
    case Mpmc:
        return mpmc_dequeue_Data(CAST(Queue_Mpmc_Data *, q), d);
    }

    return QueueResult_Error;
}

#define EXPECT(x)      \
    do {               \
        if (!(x)) {    \
            free(q);   \
            return #x; \
        }              \
    } while (0)

const char *null_pointers(Tag tag, unsigned count_in, unsigned count_out)
{
    (void) count_in;
    (void) count_out;

    QueueResult_t result = make(tag, 0, NULL, NULL);
    void *q = NULL;

    EXPECT(result == QueueResult_Error_Null_Bytes);

    return NULL;
}

const char *create(Tag tag, unsigned count_in, unsigned count_out)
{
    (void) count_in;
    (void) count_out;

    size_t bytes = -1;
    void *q = NULL;

    EXPECT(make(tag, 0, NULL, &bytes) == QueueResult_Error_Too_Small);
    EXPECT(make(tag, 1, NULL, &bytes) == QueueResult_Error_Too_Small);
    // min size

    EXPECT(make(tag, 13, NULL, &bytes) == QueueResult_Error_Not_Pow2);
    EXPECT(make(tag, 255, NULL, &bytes) == QueueResult_Error_Not_Pow2);
    // must be pow2

    EXPECT(make(tag, -1, NULL, &bytes) == QueueResult_Error_Too_Big);

    EXPECT(make(tag, -3000000, NULL, &bytes) == QueueResult_Error_Too_Big);

    EXPECT(make(tag, 1ULL << 63, NULL, &bytes) == QueueResult_Error_Too_Big);

    EXPECT(make(tag, 1ULL << 33, NULL, &bytes) == QueueResult_Error_Too_Big);
    // Insane sizes

    {
        make(tag, 1 << 8, NULL, &bytes);

        EXPECT(bytes > 0);
        EXPECT(bytes < 100000);

        void *queue = malloc(bytes);

        QueueResult_t create = make(tag, 1 << 8, q, &bytes);

        free(queue);

        EXPECT(create == QueueResult_Ok);
    }

    return NULL;
}

const char *empty(Tag tag, unsigned count_in, unsigned count_out)
{
    (void) count_in;
    (void) count_out;

    size_t bytes = 0;
    void *q = NULL;

    make(tag, 1 << 8, NULL, &bytes);

    EXPECT(bytes > 0);

    q = malloc(bytes);

    make(tag, 1 << 8, q, &bytes);

    {
        Data data = {0};

        QueueResult_t result_try_dequeue = try_dequeue(tag, q, &data);

        EXPECT(result_try_dequeue == QueueResult_Empty);
    }

    {
        Data data = {0};

        QueueResult_t result_dequeue = dequeue(tag, q, &data);

        EXPECT(result_dequeue == QueueResult_Empty);
    }

    free(q);

    return NULL;
}

const char *full(Tag tag, unsigned count_in, unsigned count_out)
{
    (void) count_in;
    (void) count_out;

    size_t bytes = 0;
    void *q = NULL;

    make(tag, 1 << 8, NULL, &bytes);

    EXPECT(bytes > 0);

    q = malloc(bytes);

    make(tag, 1 << 8, q, &bytes);

    for (unsigned i = 0; i < (1 << 8); i++) {
        Data data = {0};

        EXPECT(enqueue(tag, q, &data) == QueueResult_Ok);
    }

    {
        Data data = {0};

        QueueResult_t result_try_dequeue = try_enqueue(tag, q, &data);

        EXPECT(result_try_dequeue == QueueResult_Full);
    }

    {
        Data data = {0};

        QueueResult_t result_dequeue = enqueue(tag, q, &data);

        EXPECT(result_dequeue == QueueResult_Full);
    }

    free(q);

    return NULL;
}

typedef struct Thread_Data {
    void *q;
    int multiplier;
    Tag tag;
    atomic_size_t *global_count;
    atomic_size_t *done;
} Thread_Data;

int thread_in(void *data)
{
    Data item = {11.0f, 22, {0}};

    Thread_Data *info = CAST(Thread_Data *, data);

    unsigned max = 10000 * info->multiplier;

    for (unsigned j = 0; j < max; j++) {
        while (enqueue(info->tag, info->q, &item) != QueueResult_Ok)
            ;
    }

    atomic_fetch_sub_explicit(info->done, 1, memory_order_relaxed);

    return 0;
}

int thread_out(void *data)
{
    Thread_Data *info = CAST(Thread_Data *, data);

    unsigned max = 10000 * info->multiplier;

    for (unsigned j = 0; j < max; j++) {
        Data item = {0};
        while (dequeue(info->tag, info->q, &item) != QueueResult_Ok)
            ;

        atomic_fetch_add_explicit(info->global_count, item.b,
                                  memory_order_relaxed);
    }

    atomic_fetch_sub_explicit(info->done, 1, memory_order_relaxed);

    return 0;
}

const char *sums10000(Tag tag, unsigned count_in, unsigned count_out)
{
    void *q = NULL;
    {
        size_t bytes = 0;

        make(tag, 1 << 8, NULL, &bytes);

        EXPECT(bytes > 0);

        q = malloc(bytes);

        QueueResult_t create = make(tag, 1 << 8, q, &bytes);

        EXPECT(create == QueueResult_Ok);
    }

    thrd_t in_threads[QUEUE_TEST_THREADS_MAX];
    thrd_t out_threads[QUEUE_TEST_THREADS_MAX];

    atomic_size_t done_in_count = ATOMIC_VAR_INIT(count_in);
    atomic_size_t done_out_count = ATOMIC_VAR_INIT(count_out);
    atomic_size_t global_count = ATOMIC_VAR_INIT(0);

    int multiplier_in = QUEUE_TEST_THREADS_MAX / count_in;
    int multiplier_out = QUEUE_TEST_THREADS_MAX / count_out;

    Thread_Data data_in = {q, multiplier_in, tag, &global_count,
                           &done_in_count};
    Thread_Data data_out = {q, multiplier_out, tag, &global_count,
                            &done_out_count};

    for (unsigned i = 0; i < count_in; i++) {
        int result = thrd_create(&in_threads[i], thread_in, &data_in);
        EXPECT(result == thrd_success);
    }

    for (unsigned i = 0; i < count_out; i++) {
        int result = thrd_create(&out_threads[i], thread_out, &data_out);
        EXPECT(result == thrd_success);
    }

    while (atomic_load_explicit(&done_in_count, memory_order_relaxed))
        ;
    while (atomic_load_explicit(&done_out_count, memory_order_relaxed))
        ;

    for (unsigned i = 0; i < count_in; i++) {
        int ignored = 0;
        int result = thrd_join(in_threads[i], &ignored);

        EXPECT(result == thrd_success);
    }
    for (unsigned i = 0; i < count_out; i++) {
        int ignored = 0;
        int result = thrd_join(out_threads[i], &ignored);

        EXPECT(result == thrd_success);
    }

    size_t expected_count = 10000ULL * 22 * QUEUE_TEST_THREADS_MAX;

    EXPECT(atomic_load(&global_count) == expected_count);

    return NULL;
}

typedef const char *(*Test)(Tag, unsigned, unsigned);
#define TEST(x) \
    {           \
#x, x   \
    }

struct {
    const char *name;
    Test test;
} static tests[] = {
    TEST(null_pointers), TEST(create), TEST(empty), TEST(full), TEST(sums10000),
};

#define TEST_COUNT 5

int main(int arg_count, char **args)
{
    (void) arg_count;
    (void) args;

    struct {
        unsigned count_in;
        unsigned count_out;
    } thread_counts[(Max + 1)] = {
        {1, 1},
        {QUEUE_TEST_THREADS_MAX, 1},
        {1, QUEUE_TEST_THREADS_MAX},
        {QUEUE_TEST_THREADS_MAX, QUEUE_TEST_THREADS_MAX}};

    for (unsigned tag = 0; tag < (Max + 1); tag++) {
        for (unsigned j = 0; j < TEST_COUNT; j++) {
            const char *error = tests[j].test(tag, thread_counts[tag].count_in,
                                              thread_counts[tag].count_out);

            printf("Test: %s: %-20s: %s%s\n", tag_to_name[tag], tests[j].name,
                   (error ? "FAIL: " : "PASS"), (error ? error : ""));

            fflush(stdout);

            if (error)
                return 1;
        }
    }

    return 0;
}
