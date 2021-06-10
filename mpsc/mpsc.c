#include <stddef.h>
#include <stdint.h>

/* Classical Producer-Consumer Problem, utilizing unbounded lockless single
 * consumer multiple producer FIFO queue.
 */

typedef struct queue_internal *queue_p;

typedef enum {
    QUEUE_FALSE,
    QUEUE_SUCCESS,
    QUEUE_TRUE,
    QUEUE_OUT_OF_MEMORY = -1,
} queue_result_t;

/**
 * \brief An unbounded lockless single consumer multiple producer FIFO Queue.
 */
struct __QUEUE_API__ {
    /** Create a new Queue object.
     * @param size the storage size in bytes.
     */
    queue_p (*create)(size_t size);

    /** Push an element to the back of the queue.
     * Pushing supports copying and moving. Pushing is considered a producer
     * operation. Any thread can safely execute this operation at any time.
     * @param data the region where the value stored will be copied from.
     * @return QUEUE_OUT_OF_MEMORY if the heap is exhausted.
     */
    queue_result_t (*push)(queue_p, void *data);

    /** Check if the queue has any data.
     * The method is considered a consumer operation, and only one thread may
     * safely execute this at one time.
     * @return QUEUE_TRUE if there is a front.
     * @return QUEUE_FALSE if there is not.
     */
    queue_result_t (*hasFront)(queue_p);

    /** Get the value at the front of the queue.
     * You should always check that there is data in queue before calling
     * front as there is no built in check. If no data is in the queue when
     * front is called, memory violation likely happens.
     * Getting data is considered a consumer operation, only one thread may
     * safely execute this at one time.
     * @param data the destination where value stored will be copied to
     */
    queue_result_t (*front)(queue_p, void *data);

    /** Remove the item at the front of the queue.
     * You should always check that there is data in queue before popping as
     * there is no built in check. If no data is in the queue when pop is
     * called, memory violation likely happens.
     * Popping is considered a consumer operation, and only one thread may
     * safely execute this at one time.
     */
    queue_result_t (*pop)(queue_p);

    /** Clear the entire queue.
     * You should always clear it before deleting the Queue itself, otherwise
     * you will leak memory.
     */
    queue_result_t (*clear)(queue_p);

    /** Destroy the queue object */
    queue_result_t (*destroy)(queue_p);
} Queue;

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const size_t sentinel = 0xDEADC0DE;
static const size_t alignment = sizeof(size_t);

typedef struct node {
    atomic_uintptr_t next;
} node;

struct queue_internal {
    atomic_uintptr_t head, tail;
    size_t item_size;
};

static queue_p queue_create(size_t item_size)
{
    size_t *ptr = calloc(sizeof(struct queue_internal) + alignment, 1);
    assert(ptr);
    ptr[0] = sentinel;
    queue_p q = (queue_p)(ptr + 1);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    q->item_size = item_size;
    return q;
}

static queue_result_t queue_has_front(queue_p q)
{
    assert(q);
    return (atomic_load(&q->head) == 0) ? QUEUE_FALSE : QUEUE_TRUE;
}

static queue_result_t queue_front(queue_p q, void *data)
{
    assert(q);
    assert(data);
    node *head = (node *) atomic_load(&q->head);
    assert(head);
    memcpy(data, (void *) (head + 1), q->item_size);
    return QUEUE_SUCCESS;
}

static queue_result_t queue_pop(queue_p q)
{
    assert(q);
    assert(queue_has_front(q) == QUEUE_TRUE);

    /* get the head */
    node *popped = (node *) atomic_load(&q->head);
    node *compare = popped;

    /* set the tail and head to nothing if they are the same */
    if (atomic_compare_exchange_strong(&q->tail, &compare, 0)) {
        compare = popped;
        /* It is possible for another thread to have pushed after
         * we swap out the tail. In this case, the head will be different
         * then what was popped, so we just do a blind exchange regardless
         * of the result.
         */
        atomic_compare_exchange_strong(&q->head, &compare, 0);
    } else {
        /* tail is different from head, set the head to the next value */
        node *new_head = 0;
        while (!new_head) {
            /* It is possible that the next node has not been assigned yet,
             * so just spin until the pushing thread stores the value.
             */
            new_head = (node *) atomic_load(&popped->next);
        }
        atomic_store(&q->head, (atomic_uintptr_t) new_head);
    }

    free(popped);
    return QUEUE_SUCCESS;
}

static queue_result_t queue_push(queue_p q, void *data)
{
    assert(q);
    /* create the new tail */
    node *new_tail = malloc(sizeof(node) + q->item_size);
    if (!new_tail)
        return QUEUE_OUT_OF_MEMORY;

    atomic_init(&new_tail->next, 0);
    memcpy(new_tail + 1, data, q->item_size);

    /* swap the new tail with the old */
    node *old_tail =
        (node *) atomic_exchange(&q->tail, (atomic_uintptr_t) new_tail);

    /* link the old tail to the new */
    atomic_store(old_tail ? &old_tail->next : &q->head,
                 (atomic_uintptr_t) new_tail);
    return QUEUE_SUCCESS;
}

static queue_result_t queue_clear(queue_p q)
{
    assert(q);
    while (queue_has_front(q) == QUEUE_TRUE) {
        queue_result_t result = queue_pop(q);
        assert(result == QUEUE_SUCCESS);
    }
    return QUEUE_SUCCESS;
}

static queue_result_t queue_destroy(queue_p q)
{
    size_t *ptr = (size_t *) q - 1;
    assert(ptr[0] == sentinel);
    free(ptr);
    return QUEUE_SUCCESS;
}

/* API gateway */
struct __QUEUE_API__ Queue = {
    .create = queue_create,
    .hasFront = queue_has_front,
    .front = queue_front,
    .pop = queue_pop,
    .push = queue_push,
    .clear = queue_clear,
    .destroy = queue_destroy,
};

#include <pthread.h>
#include <stdio.h>

static void basic_test()
{
    queue_p q = Queue.create(sizeof(int));
    assert(q);

    /* initial queue is empty */
    assert(Queue.hasFront(q) == QUEUE_FALSE);

    queue_result_t result;
    /* push one item to the empty queue */
    {
        int in = 1, out = 0;
        {
            result = Queue.push(q, &in);
            assert(result == QUEUE_SUCCESS);
        }
        assert(Queue.hasFront(q) == QUEUE_TRUE);
        {
            result = Queue.front(q, &out);
            assert(result == QUEUE_SUCCESS);
        }
        assert(out == in);
    }

    /* pop one item out of a single item queue */
    {
        result = Queue.pop(q);
        assert(result == QUEUE_SUCCESS);
    }
    assert(Queue.hasFront(q) == QUEUE_FALSE);

    /* push many items on the queue */
    for (size_t i = 0; i < 64; ++i) {
        int in = i, out = -1;
        result = Queue.push(q, &in);
        assert(result == QUEUE_SUCCESS);

        assert(Queue.hasFront(q) == QUEUE_TRUE);
        result = Queue.front(q, &out);
        assert(result == QUEUE_SUCCESS);

        assert(out == 0);
    }

    /* pop many items from the queue */
    for (size_t i = 0; i < 32; ++i) {
        int out = -1;
        result = Queue.front(q, &out);
        assert(result == QUEUE_SUCCESS);
        assert(out == i);

        result = Queue.pop(q);
        assert(result == QUEUE_SUCCESS);
    }

    /* clear the queue */
    assert(Queue.hasFront(q) == QUEUE_TRUE);
    result = Queue.clear(q);
    assert(result == QUEUE_SUCCESS);

    assert(Queue.hasFront(q) == QUEUE_FALSE);

    Queue.destroy(q);
}

#define THREAD_COUNT (50 * 1.5 * 1000 * 1000)
#define PRODUCER_COUNT 64

typedef struct {
    atomic_int consume_count, producer_count;
    queue_p q;
} queue_test_t;

static void *test_consumer(void *arg)
{
    queue_test_t *test = (queue_test_t *) arg;
    while (atomic_load(&test->consume_count) < THREAD_COUNT) {
        if (Queue.hasFront(test->q)) {
            atomic_fetch_add(&test->consume_count, 1);
            queue_result_t result = Queue.pop(test->q);
            assert(result == QUEUE_SUCCESS);
        }
    }
    return NULL;
}

static void *test_producer(void *arg)
{
    queue_test_t *test = (queue_test_t *) arg;
    assert(test->q);
    while (1) {
        int in = atomic_fetch_add(&test->producer_count, 1);
        if (in >= THREAD_COUNT)
            break;
        queue_result_t result = Queue.push(test->q, &in);
        assert(result == QUEUE_SUCCESS);
    }
    return NULL;
}

static void stress_test()
{
    queue_test_t test;
    atomic_init(&test.consume_count, 0);
    atomic_init(&test.producer_count, 0);

    test.q = Queue.create(sizeof(int));
    assert(test.q);

    /* thread creation */
    pthread_t consumer, producers[PRODUCER_COUNT];
    {
        int p_result = pthread_create(&consumer, NULL, test_consumer, &test);
        assert(p_result == 0);
    }
    for (size_t i = 0; i < PRODUCER_COUNT; ++i) {
        int p_result =
            pthread_create(&producers[i], NULL, test_producer, &test);
        assert(p_result == 0);
    }

    /* wait for completion */
    for (size_t i = 0; i < PRODUCER_COUNT; ++i) {
        int p_result = pthread_join(producers[i], NULL);
        assert(p_result == 0);
    }
    {
        int p_result = pthread_join(consumer, NULL);
        assert(p_result == 0);
    }

    assert(Queue.hasFront(test.q) == QUEUE_FALSE);

    Queue.destroy(test.q);
}

int main(int argc, char *argv[])
{
    printf("** Basic operations **\n");
    basic_test();
    printf("Verified OK!\n\n");

    printf("** Stress test **\n");
    stress_test();
    printf("Verified OK!\n\n");

    return 0;
}
