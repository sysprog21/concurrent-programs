/* A concurrent single-producer, multiple-consumer (SPMC) queue using C11
 * Atomics. It is lock-free and atomic, allowing one enqueue-caller/producer,
 * arbitrary amount of dequeue-callers/consumers.
 *
 * Known issue: if one has multiple consumers, some of them will be swapped
 * off the CPU after grabbing curr_dequeue, and will have dequeued an element
 * from a different node, if that node ends up having free space.
 */

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct __spmc_node {
    size_t cap; /* One more than the number of slots available */
    _Atomic size_t front, back;
    struct __spmc_node *_Atomic next;
    uintptr_t buf[];
} spmc_node_t;

typedef void (*spmc_destructor_t)(uintptr_t);
struct spmc_base {
    /* current node which enqueues/dequeues */
    spmc_node_t *_Atomic curr_enqueue, *_Atomic curr_dequeue;
    uint8_t last_power;
    spmc_destructor_t destructor;
};
typedef struct spmc_base *spmc_ref_t;

#define DEFAULT_INITIAL_POWER 6 /* Initial capacity: 64, as a power of two */

#define SIZE_FROM_CAP(cap, offset) ((cap) * sizeof(uintptr_t) + (offset))

#define MODULO(lhs, rhs) ((lhs) & (rhs - 1)) /* Requires rhs is power of 2 */
#define INDEX_OF(idx, node) (MODULO((idx), (node)->cap))
#define IS_READABLE(idx, node) ((node)->back - (idx) != 0)
#define IS_WRITABLE(idx, node) ((idx) - (node)->front < (node)->cap)

/* The head of the spmc resides contiguously after the spmc_base struct itself.
 * Here, two objects are stored in the same block of memory, but are accessed
 * separately.
 */
#define HEAD_OF(spmc) ((spmc_node_t *) (void *) ((spmc_ref_t)(spmc) + 1))

static void init_node(spmc_node_t *node, spmc_node_t *next, size_t cap)
{
    node->cap = cap;
    atomic_init(&node->front, 0), atomic_init(&node->back, 0);
    atomic_init(&node->next, next);
}

/* In the event initial_cap is 0, the spmc will select a default capacity.
 * Takes capacities as powers of two. i.e., initial_cap argument of 4 =>
 * an allocation of ~16 machine words.
 */
spmc_ref_t spmc_new(size_t initial_cap, spmc_destructor_t destructor)
{
    assert(initial_cap < sizeof(size_t) * CHAR_BIT);
    const uint8_t power = initial_cap ? initial_cap : DEFAULT_INITIAL_POWER;
    const size_t cap = 1 << power;

    /* Allocate spmc_base and head spmc_node in the same underlying buffer */
    spmc_ref_t spmc = malloc(
        SIZE_FROM_CAP(cap, sizeof(struct spmc_base) + sizeof(spmc_node_t)));
    spmc_node_t *const head = HEAD_OF(spmc);
    init_node(head, head, cap);

    atomic_init(&spmc->curr_enqueue, head);
    atomic_init(&spmc->curr_dequeue, head);
    spmc->destructor = destructor;
    spmc->last_power = power;

    return spmc;
}

/* Destroy the SPMC, freeing all nodes/elements now assoicated with it.
 * Assume all users of the channel are done with it.
 */
void spmc_delete(spmc_ref_t spmc)
{
    const spmc_node_t *const head = HEAD_OF(spmc);
    spmc_node_t *prev;
    if (spmc->destructor) {
        for (spmc_node_t *node = head->next; node != head;
             prev = node, node = node->next, free(prev))
            for (size_t i = node->front; IS_READABLE(i, node); ++i)
                spmc->destructor(node->buf[i]);
    } else {
        for (spmc_node_t *node = head->next; node != head;
             prev = node, node = node->next, free(prev))
            ;
    }
    /* Also frees the head; it resides reside in the same buffer. */
    free(spmc);
}

/* Send (enqueue) an item onto the SPMC */
bool spmc_enqueue(spmc_ref_t spmc, uintptr_t element)
{
    spmc_node_t *node =
        atomic_load_explicit(&spmc->curr_enqueue, memory_order_relaxed);
    size_t idx;
retry:
    idx = atomic_load_explicit(&node->back, memory_order_consume);
    if (!IS_WRITABLE(idx, node)) {
        spmc_node_t *const next =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        /* Never move to write on top of the node that is currently being read;
         * In that case, items would be read out of order they were enqueued.
         */
        if (next !=
            atomic_load_explicit(&spmc->curr_dequeue, memory_order_relaxed)) {
            node = next;
            goto retry;
        }

        const uint8_t power = ++spmc->last_power;
        assert(power < sizeof(size_t) * CHAR_BIT);
        const size_t cap = 1 << power;
        spmc_node_t *new_node = malloc(SIZE_FROM_CAP(cap, sizeof(spmc_node_t)));
        if (!new_node)
            return false;

        init_node(new_node, next, cap);
        atomic_store_explicit(&node->next, new_node, memory_order_release);
        idx = 0;
        node = new_node;
    }
    node->buf[INDEX_OF(idx, node)] = element;
    atomic_store_explicit(&spmc->curr_enqueue, node, memory_order_relaxed);
    atomic_fetch_add_explicit(&node->back, 1, memory_order_release);
    return true;
}

/* Recieve (dequeue) an item from the SPMC */
bool spmc_dequeue(spmc_ref_t spmc, uintptr_t *slot)
{
    spmc_node_t *node =
        atomic_load_explicit(&spmc->curr_dequeue, memory_order_consume);
    size_t idx;
no_increment:
    do {
        idx = atomic_load_explicit(&node->front, memory_order_consume);
        if (!IS_READABLE(idx, node)) {
            if (node != spmc->curr_enqueue)
                atomic_compare_exchange_strong(
                    &spmc->curr_dequeue, &node,
                    atomic_load_explicit(&node->next, memory_order_relaxed));
            goto no_increment;
        } else
            *slot = node->buf[INDEX_OF(idx, node)];
    } while (
        !atomic_compare_exchange_weak(&node->front, &(size_t){idx}, idx + 1));
    return true;
}

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define N_ITEMS (1024UL * 8)
static void *producer_thread(void *arg)
{
    spmc_ref_t spmc = arg;
    for (uintptr_t i = 0; i < N_ITEMS; ++i) {
        if (!spmc_enqueue(spmc, i))
            fprintf(stderr, "Failed to enqueue on %zu.\n", (size_t) i);
    }
    return NULL;
}

#define N_MC_ITEMS (1024UL * 8)
static _Atomic size_t observed_count[N_MC_ITEMS + 1];

static void *mc_thread(void *arg)
{
    spmc_ref_t spmc = arg;
    uintptr_t element = 0, greatest = 0;

    for (;;) {
        greatest = (greatest > element) ? greatest : element;
        if (!spmc_dequeue(spmc, &element))
            fprintf(stderr, "Failed to dequeue in mc_thread.\n");
        else if (observed_count[element]++)
            fprintf(stderr, "Consumed twice!\n");
        else if (element < greatest)
            fprintf(stderr, "%zu after %zu; bad order!\n", (size_t) element,
                    (size_t) greatest);
        printf("Observed %zu.\n", (size_t) element);

        /* Test for sentinel signalling termination */
        if (element >= (N_MC_ITEMS - 1)) {
            spmc_enqueue(spmc, element + 1); /* notify other threads */
            break;
        }
    }
    return NULL;
}

#define N_MC_THREADS 16
int main()
{
    spmc_ref_t spmc = spmc_new(0, NULL);
    pthread_t mc[N_MC_THREADS], producer;

    pthread_create(&producer, NULL, producer_thread, spmc);
    for (int i = 0; i < N_MC_THREADS; i++)
        pthread_create(&mc[i], NULL, mc_thread, spmc);

    pthread_join(producer, NULL);
    for (int i = 0; i < N_MC_THREADS; i++)
        pthread_join(mc[i], NULL);

    for (size_t i = 0; i < N_MC_ITEMS; ++i) {
        if (observed_count[i] == 1)
            continue;
        fprintf(stderr, "An item seen %zu times: %zu.\n", observed_count[i], i);
    }
    spmc_delete(spmc);
    return 0;
}
