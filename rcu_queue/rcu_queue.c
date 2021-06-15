#define CACHE_LINE_SIZE 64

#define ALLOC_SIZE(...) __attribute__((__alloc_size__(__VA_ARGS__)))
#define ALLOC_ALIGN(n) __attribute__((__alloc_align__(n)))

#define LIKELY(exp) __builtin_expect((exp), 1)
#define UNLIKELY(exp) __builtin_expect((exp), 0)

#include <assert.h>
#include <stdlib.h>

#include <stdint.h>

typedef void *(*wrap_realloc_t)(void *ptr, size_t size);
static wrap_realloc_t wrap_realloc_func = &realloc;

ALLOC_SIZE(1)
static inline void *wrap_malloc(size_t size)
{
    void *ptr = wrap_realloc_func(NULL, size);

    /* The returned pointer should be at least aligned to sizeof(void *). */
    assert((uintptr_t) ptr % sizeof(void *) == 0);

    return ptr;
}

static inline void wrap_free(void *ptr)
{
    if (ptr)
        wrap_realloc_func(ptr, 0);
}

#include <stdatomic.h>
#include <stdbool.h>

struct qsbr_entry {
    _Atomic(struct qsbr_entry *) next;
};

struct qsbr_global {
    /* Current global epoch */
    _Atomic uint32_t epoch;

    /* Number of threads that have not entered a quiescent section in the
     * current epoch yet.
     */
    _Atomic uint32_t n_remaining;

    /* Entries that can be freed at current epoch + 1 */
    _Atomic(struct qsbr_entry *) to_free1;

    /* Entries that can be freed at current epoch + 2 */
    _Atomic(struct qsbr_entry *) to_free2;

    uint32_t n_threads;
};

struct qsbr_local {
    uint32_t epoch;
};

static inline void qsbr_init_global(struct qsbr_global *global,
                                    uint32_t n_threads)
{
    atomic_init(&global->epoch, 0);
    atomic_init(&global->n_remaining, 0);
    atomic_init(&global->to_free1, NULL);
    atomic_init(&global->to_free2, NULL);
    global->n_threads = n_threads;
}

static inline void qsbr_init_local(struct qsbr_local *local)
{
    local->epoch = 0;
}

static inline void qsbr_fini_global(struct qsbr_global *global,
                                    struct qsbr_entry **to_free1,
                                    struct qsbr_entry **to_free2)
{
    *to_free1 = atomic_load_explicit(&global->to_free1, memory_order_relaxed);
    *to_free2 = atomic_load_explicit(&global->to_free2, memory_order_relaxed);
}

static inline void qsbr_free(struct qsbr_global *global,
                             struct qsbr_local *local,
                             struct qsbr_entry *entry)
{
    struct qsbr_entry *expected = NULL;

    /* qsbr_free() is not supported when the number of threads is 1. */
    assert(global->n_threads > 1);

    atomic_store_explicit(&entry->next, NULL, memory_order_relaxed);
    bool exchanged = atomic_compare_exchange_strong_explicit(
        &global->to_free1, &expected, entry, memory_order_acq_rel,
        memory_order_consume);
    if (UNLIKELY(exchanged)) {
        uint32_t epoch =
            atomic_load_explicit(&global->epoch, memory_order_relaxed);
        atomic_store_explicit(&global->n_remaining, global->n_threads - 1,
                              memory_order_relaxed);
        atomic_store_explicit(&global->epoch, epoch + 1, memory_order_release);
        local->epoch = epoch + 1;
    } else {
        struct qsbr_entry *next =
            atomic_load_explicit(&global->to_free2, memory_order_relaxed);
        atomic_store_explicit(&entry->next, next, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(
            &global->to_free2, &entry->next, entry, memory_order_release,
            memory_order_relaxed))
            ;
    }
}

static inline struct qsbr_entry *qsbr_quiescent(struct qsbr_global *global,
                                                struct qsbr_local *local)
{
    uint32_t epoch = atomic_load_explicit(&global->epoch, memory_order_consume);
    if (LIKELY(epoch == local->epoch))
        return NULL;

    struct qsbr_entry *to_free = NULL;
    uint32_t n_remaining = atomic_fetch_sub_explicit(&global->n_remaining, 1,
                                                     memory_order_acq_rel);
    assert(n_remaining >= 1);

    if (UNLIKELY(n_remaining == 1)) {
        to_free = atomic_load_explicit(&global->to_free1, memory_order_acquire);
        assert(to_free);

        struct qsbr_entry *new_to_free1 =
            atomic_load_explicit(&global->to_free2, memory_order_consume);
        if (new_to_free1) {
            while (!atomic_compare_exchange_weak_explicit(
                &global->to_free2, &new_to_free1, NULL, memory_order_acquire,
                memory_order_relaxed))
                ;
            assert(new_to_free1);
            atomic_store_explicit(&global->to_free1, new_to_free1,
                                  memory_order_relaxed);
            atomic_store_explicit(&global->n_remaining, global->n_threads - 1,
                                  memory_order_relaxed);
            atomic_store_explicit(&global->epoch, epoch + 1,
                                  memory_order_release);
            epoch++;
        } else {
            atomic_store_explicit(&global->to_free1, NULL,
                                  memory_order_relaxed);
        }
    }

    local->epoch = epoch;
    return to_free;
}

#include <stdalign.h>

struct qsbr_queue_node {
    struct qsbr_entry qsbr_entry;
    void *value;
    _Atomic(struct qsbr_queue_node *) next;
};

struct qsbr_queue {
    alignas(CACHE_LINE_SIZE) _Atomic(struct qsbr_queue_node *) head;
    alignas(CACHE_LINE_SIZE) _Atomic(struct qsbr_queue_node *) tail;
};

static inline void qsbr_queue_init(struct qsbr_queue *queue,
                                   struct qsbr_queue_node *init_node)
{
    atomic_init(&init_node->next, NULL);
    atomic_init(&queue->head, init_node);
    atomic_init(&queue->tail, init_node);
}

static inline void qsbr_queue_fini(struct qsbr_queue *queue,
                                   struct qsbr_queue_node **released_node)
{
    *released_node = atomic_load_explicit(&queue->head, memory_order_relaxed);
}

static inline void qsbr_queue_push(struct qsbr_queue *queue,
                                   struct qsbr_queue_node *node,
                                   void *value)
{
    node->value = value;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    struct qsbr_queue_node *tail =
        atomic_load_explicit(&queue->tail, memory_order_relaxed);
    for (;;) {
        struct qsbr_queue_node *next = NULL;
        if (atomic_compare_exchange_weak_explicit(&tail->next, &next, node,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            break;
        atomic_compare_exchange_weak_explicit(&queue->tail, &tail, next,
                                              memory_order_relaxed,
                                              memory_order_relaxed);
    }
    atomic_compare_exchange_strong(&queue->tail, &tail, node);
}

static inline bool qsbr_queue_pop(struct qsbr_queue *queue,
                                  struct qsbr_queue_node **released_node,
                                  void **value_ptr)
{
    struct qsbr_queue_node *next;

    struct qsbr_queue_node *head =
        atomic_load_explicit(&queue->head, memory_order_consume);
    for (;;) {
        next = atomic_load_explicit(&head->next, memory_order_acquire);
        if (!next)
            return false;
        if (atomic_compare_exchange_weak_explicit(&queue->head, &head, next,
                                                  memory_order_acquire,
                                                  memory_order_consume))
            break;
    }

    *value_ptr = next->value;
    *released_node = head;
    return true;
}

#include <errno.h>
#include <pthread.h>

/* Test program starts here */

#define container_of(ptr, type, member) \
    ((type *) ((uint8_t *) (ptr) -offsetof(type, member)))

/* Lehmer RNG */
#define RANDOM_NEXT(r) ((uint32_t)((uint64_t)(r) *48271 % UINT64_C(2147483647)))

#define FATAL(fmt, ...)                           \
    do {                                          \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        abort();                                  \
    } while (0)

#define CHECK(expr, fmt, ...)                                            \
    do {                                                                 \
        if (!(expr))                                                     \
            FATAL("Asseration \"%s\" failed in %s (%s:%u): " fmt, #expr, \
                  __func__, __FILE__, __LINE__, ##__VA_ARGS__);          \
    } while (0)

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

static uint32_t n_workers, n_tries;
static struct qsbr_queue queue;
static atomic_uint barrier;
static _Atomic uint64_t total_sum;
static struct qsbr_global qsbr_global;

static struct qsbr_queue_node *alloc_node(void)
{
    struct qsbr_queue_node *node = wrap_malloc(sizeof(*node));
    CHECK(node, "Allocating node failed");

    CHECK((uintptr_t) node % 8 == 0, "Bad alignment");

    return node;
}

static void free_node(struct qsbr_queue_node *node)
{
    wrap_free(node);
}

static void free_qsbr_nodes(struct qsbr_entry *head)
{
    while (head) {
        struct qsbr_entry *next =
            atomic_load_explicit(&head->next, memory_order_relaxed);
        free_node(container_of(head, struct qsbr_queue_node, qsbr_entry));
        head = next;
    }
}

static void *worker(void *arg)
{
    uint64_t sum = 0;
    uint32_t n_enqueue = 0, n_dequeue = 0;
    uint32_t r = (uint32_t)(uintptr_t) arg;

    struct qsbr_local qsbr_local;
    qsbr_init_local(&qsbr_local);

    /* Wait until all threads are created. */
    atomic_fetch_sub(&barrier, 1);
    while (atomic_load_explicit(&barrier, memory_order_acquire) > 0)
        ;

    while (n_enqueue < n_tries || n_dequeue < n_tries) {
        r = RANDOM_NEXT(r);
        uint32_t iters = r % 1024;
        while (iters-- > 0 && n_enqueue < n_tries) {
            struct qsbr_queue_node *node = alloc_node();
            qsbr_queue_push(&queue, node, (void *) (uintptr_t)(n_enqueue + 1));
            n_enqueue++;
        }

        r = RANDOM_NEXT(r);
        iters = r % 1024;
        while (iters-- > 0 && n_dequeue < n_tries) {
            struct qsbr_queue_node *node;
            void *value;
            bool popped = qsbr_queue_pop(&queue, &node, &value);
            if (!popped)
                continue;
            if (n_workers == 1)
                free_node(node);
            else
                qsbr_free(&qsbr_global, &qsbr_local, &node->qsbr_entry);
            sum += (uint32_t)(uintptr_t) value;
            n_dequeue++;
        }

        struct qsbr_entry *to_free = qsbr_quiescent(&qsbr_global, &qsbr_local);
        free_qsbr_nodes(to_free);
    }

    atomic_fetch_add(&total_sum, sum);

    return NULL;
}

#include <unistd.h>

int main(void)
{
    uint32_t seed = getpid() ^ (uintptr_t) &main;
    n_workers = 32;
    n_tries = 500;

    pthread_t *threads = wrap_malloc(n_workers * sizeof(pthread_t));
    if (!threads) {
        fputs("Allocating memory for threads failed\n", stderr);
        return 1;
    }

    qsbr_init_global(&qsbr_global, n_workers);

    struct qsbr_queue_node *node = alloc_node();
    qsbr_queue_init(&queue, node);

    atomic_store(&barrier, n_workers);

    uint32_t r = seed;
    for (uint32_t i = 0; i < n_workers; i++) {
        r = RANDOM_NEXT(r);
        int err =
            pthread_create(&threads[i], NULL, worker, (void *) (uintptr_t) r);
        if (err != 0) {
            fprintf(stderr, "Creating thread failed, err=%d\n", err);
            return 1;
        }
    }

    for (uint32_t i = 0; i < n_workers; i++)
        pthread_join(threads[i], NULL);

    wrap_free(threads);

    qsbr_queue_fini(&queue, &node);
    free_node(node);

    struct qsbr_entry *to_free1, *to_free2;
    qsbr_fini_global(&qsbr_global, &to_free1, &to_free2);
    free_qsbr_nodes(to_free1);
    free_qsbr_nodes(to_free2);

    uint64_t expected_sum =
        ((uint64_t) n_tries * ((uint64_t) n_tries + 1) / 2) * n_workers;
    printf("sum: %" PRIu64 ", expected: %" PRIu64 "\n", atomic_load(&total_sum),
           expected_sum);

    return total_sum != expected_sum;
}
