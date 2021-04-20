#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096     /* FIXME: avoid hard-coded */
#define CACHE_LINE_SIZE 64 /* FIXME: make it configurable */
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))
#define DOUBLE_CACHE_ALIGNED __attribute__((aligned(2 * CACHE_LINE_SIZE)))

static inline void *align_malloc(size_t align, size_t size)
{
    void *ptr;
    int ret = posix_memalign(&ptr, align, size);
    if (ret != 0) {
        perror(strerror(ret));
        abort();
    }
    return ptr;
}

#define N (1 << 12) /* node size */
#define NBITS (N - 1)

typedef struct __node {
    struct __node *volatile next DOUBLE_CACHE_ALIGNED;
    long id DOUBLE_CACHE_ALIGNED;
    void *volatile cells[N] DOUBLE_CACHE_ALIGNED;
} node_t;

#define HANDLES 128 /* support 127 threads */

typedef struct {
    node_t *spare;

    node_t *volatile put_node CACHE_ALIGNED;
    node_t *volatile pop_node CACHE_ALIGNED;
} handle_t;

typedef struct {
    node_t *init_node;
    volatile long init_id DOUBLE_CACHE_ALIGNED;

    volatile long put_index DOUBLE_CACHE_ALIGNED;
    volatile long pop_index DOUBLE_CACHE_ALIGNED;

    handle_t *volatile enq_handles[HANDLES];
    handle_t *volatile deq_handles[HANDLES];

    int threshold;

    pthread_barrier_t enq_barrier, deq_barrier;
} mpmc_t;

static inline node_t *mpmc_new_node()
{
    node_t *n = align_malloc(PAGE_SIZE, sizeof(node_t));
    memset(n, 0, sizeof(node_t));
    return n;
}

enum queue_ops {
    ENQ = 1 << 1,
    DEQ = 1 << 0,
};

/* regiseter the enqueuers first, dequeuers second. */
void mpmc_queue_register(mpmc_t *q, handle_t *th, int flag)
{
    th->spare = mpmc_new_node();
    th->put_node = th->pop_node = q->init_node;

    if (flag & ENQ) {
        handle_t **tail = q->enq_handles;
        for (int i = 0;; ++i) {
            handle_t *init = NULL;
            if (!tail[i] &&
                __atomic_compare_exchange_n(tail + i, &init, th, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
        /* wait for the other enqueuers to register */
        pthread_barrier_wait(&q->enq_barrier);
    }

    if (flag & DEQ) {
        handle_t **tail = q->deq_handles;
        for (int i = 0;; ++i) {
            handle_t *init = NULL;
            if (!tail[i] &&
                __atomic_compare_exchange_n(tail + i, &init, th, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
        /* wait for the other dequeuers to register */
        pthread_barrier_wait(&q->deq_barrier);
    }
}

void mpmc_init_queue(mpmc_t *q, int enqs, int deqs, int threshold)
{
    q->init_node = mpmc_new_node();
    q->threshold = threshold;
    q->put_index = q->pop_index = q->init_id = 0;

    pthread_barrier_init(&q->enq_barrier, NULL, enqs); /* enqueuers */
    pthread_barrier_init(&q->deq_barrier, NULL, deqs); /* dequeuers */
}

/* locate the offset on the nodes and nodes needed. */
static void *mpmc_find_cell(node_t *volatile *ptr, long i, handle_t *th)
{
    node_t *curr = *ptr; /* get current node */

    /* j is thread's local node'id (put node or pop node), (i / N) is the cell
     * needed node'id. and we shoud take it, By filling the nodes between the j
     * and (i / N) through 'next' field
     */
    for (long j = curr->id; j < i / N; ++j) {
        node_t *next = curr->next;
        if (!next) { /* start filling */
            /* use thread's standby node */
            node_t *tmp = th->spare;
            if (!tmp) {
                tmp = mpmc_new_node();
                th->spare = tmp;
            }

            tmp->id = j + 1; /* next node's id */

            /* if true, then use this thread's node, else then thread has have
             * done this.
             */
            /* __atomic_compare_exchange_n(ptr, cmp, val, 0, __ATOMIC_RELEASE,
             * __ATOMIC_ACQUIRE) is an atomic compare-and-swap that ensures
             * release semantic when succeed or acquire semantic when failed.
             */
            if (__atomic_compare_exchange_n(&curr->next, &next, tmp, 0,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_ACQUIRE)) {
                next = tmp;
                th->spare = NULL; /* now thread there is no standby node */
            }
        }
        curr = next; /* take the next node */
    }
    *ptr = curr; /* update our node to the present node */

    /* Orders processor execution, so other thread can see the '*ptr = curr' */
    __asm("sfence" ::: "cc", "memory"); /* FIXME: x86-only */

    /* now we get the needed cell, its' node is curr and index is i % N */
    return &curr->cells[i & N];
}

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
static inline int mpmc_futex_wake(void *addr, int val)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, val, NULL, NULL, 0);
}

static inline int mpmc_futex_wait(void *addr, int val)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

void mpmc_enqueue(mpmc_t *q, handle_t *th, void *v)
{
    /* return the needed index */
    void *volatile *c = mpmc_find_cell(
        &th->put_node, __atomic_fetch_add(&q->put_index, 1, __ATOMIC_SEQ_CST),
        th);
    /* __atomic_fetch_add(ptr, val) is an atomic fetch-and-add that also
     * ensures sequential consistency
     */

    /* now c is the needed cell */
    void *cv;

    /* if XCHG (ATOMIC: Exchange Register/Memory with Register) return NULL,
     * so our value has put into the cell, just return.
     */
    if ((cv = __atomic_exchange_n(c, v, __ATOMIC_ACQ_REL)) == NULL)
        return;

    /* else the couterpart pop thread has wait this cell, so we just change the
     * wating value to 0 and wake it
     */
    *((int *) cv) = 0;
    mpmc_futex_wake(cv, 1);
}

void *mpmc_dequeue(mpmc_t *q, handle_t *th)
{
    void *cv;
    int futex_addr = 1;

    /* the needed pop_index */
    long index = __atomic_fetch_add(&q->pop_index, 1, __ATOMIC_SEQ_CST);

    /* locate the needed cell */
    void *volatile *c = mpmc_find_cell(&th->pop_node, index, th);

    /* because the queue is a blocking queue, so we just use more spin. */
    int times = (1 << 20);
    do {
        cv = *c;
        if (cv)
            goto over;
        __asm__("pause"); /* FIXME: x86-only */
    } while (times-- > 0);

    /* XCHG, if return NULL so this cell is NULL, we just wait and observe the
     * futex_addr's value to 0.
     */
    if ((cv = __atomic_exchange_n(c, &futex_addr, __ATOMIC_ACQ_REL)) == NULL) {
        /* call wait before compare futex_addr to prevent use-after-free of
         * futex_addr at mpmc_enqueue(call wake)
         */
        do {
            mpmc_futex_wait(&futex_addr, 1);
        } while (futex_addr == 1);

        /* the couterpart put thread has change futex_addr's value to 0. and the
         * data has into cell(c).
         */
        cv = *c;
    }

over:
    /* if the index is the node's last cell: (NBITS == 4095), it Try to reclaim
     * the memory. so we just take the smallest ID node that is not
     * reclaimed(init_node), and At the same time, by traversing the local data
     * of other threads, we get a larger ID node(min_node). So it is safe to
     * recycle the memory [init_node, min_node).
     */
    if ((index & NBITS) == NBITS) {
        /* __atomic_load_n(ptr, __ATOMIC_ACQUIRE) is a load with a following
         * acquire fence to ensure no following load and stores can start before
         * the current load completes.
         */
        long init_index = __atomic_load_n(&q->init_id, __ATOMIC_ACQUIRE);

        /* __atomic_compare_exchange_n(ptr, cmp, val, 0, __ATOMIC_ACQUIRE,
         * __ATOMIC_RELAXED) is an atomic compare-and-swap that ensures acquire
         * semantic when succeed or relaxed semantic when failed.
         */
        if ((th->pop_node->id - init_index) >= q->threshold &&
            init_index >= 0 &&
            __atomic_compare_exchange_n(&q->init_id, &init_index, -1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            node_t *init_node = q->init_node;
            th = q->deq_handles[0];
            node_t *min_node = th->pop_node;

            int i;
            handle_t *next = q->deq_handles[i = 1];
            while (next) {
                node_t *next_min = next->pop_node;
                if (next_min->id < min_node->id)
                    min_node = next_min;
                if (min_node->id <= init_index)
                    break;
                next = q->deq_handles[++i];
            }

            next = q->enq_handles[i = 0];
            while (next) {
                node_t *next_min = next->put_node;
                if (next_min->id < min_node->id)
                    min_node = next_min;
                if (min_node->id <= init_index)
                    break;
                next = q->enq_handles[++i];
            }

            long new_id = min_node->id;
            if (new_id <= init_index)
                /* __atomic_store_n(ptr, val, __ATOMIC_RELEASE) is a store with
                 * a preceding release fence to ensure all previous load and
                 * stores completes before the current store is visiable.
                 */
                __atomic_store_n(&q->init_id, init_index, __ATOMIC_RELEASE);
            else {
                q->init_node = min_node;
                __atomic_store_n(&q->init_id, new_id, __ATOMIC_RELEASE);

                do {
                    node_t *tmp = init_node->next;
                    free(init_node);
                    init_node = tmp;
                } while (init_node != min_node);
            }
        }
    }
    return cv;
}

#include <sys/time.h>

static long COUNTS_PER_THREAD = 2500000;
static int threshold = 8;
static mpmc_t mpmc;

static pthread_barrier_t prod_barrier, cons_barrier;

static void *producer(void *index)
{
    mpmc_t *q = &mpmc;
    handle_t *th = malloc(sizeof(handle_t));
    memset(th, 0, sizeof(handle_t));
    mpmc_queue_register(q, th, ENQ);

    for (;;) {
        pthread_barrier_wait(&prod_barrier);
        for (int i = 0; i < COUNTS_PER_THREAD; ++i)
            mpmc_enqueue(q, th, 1 + i + ((int) index) * COUNTS_PER_THREAD);
        pthread_barrier_wait(&prod_barrier);
    }
    return NULL;
}

#define THREAD_NUM 4
static bool *array;
static void *consumer(void *index)
{
    mpmc_t *q = &mpmc;
    handle_t *th = malloc(sizeof(handle_t));
    memset(th, 0, sizeof(handle_t));
    mpmc_queue_register(q, th, DEQ);

    for (;;) {
        pthread_barrier_wait(&cons_barrier);
        for (long i = 0; i < COUNTS_PER_THREAD; ++i) {
            int value;
            if (!(value = mpmc_dequeue(q, th)))
                return NULL;
            array[value] = true;
        }
        pthread_barrier_wait(&cons_barrier);
    }

    fflush(stdout);
    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_barrier_init(&prod_barrier, NULL, THREAD_NUM + 1);
    pthread_barrier_init(&cons_barrier, NULL, THREAD_NUM + 1);
    if (argc >= 3) {
        COUNTS_PER_THREAD = atol(argv[1]);
        threshold = atoi(argv[2]);
    }

    printf("Amount: %ld\n", THREAD_NUM * COUNTS_PER_THREAD);
    fflush(stdout);
    array = malloc((1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(bool));
    memset(array, 0, (1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(bool));
    mpmc_init_queue(&mpmc, THREAD_NUM, THREAD_NUM, threshold);

    pthread_t pids[THREAD_NUM];

    for (int i = 0; i < THREAD_NUM; ++i) {
        if (-1 == pthread_create(&pids[i], NULL, producer, i) ||
            -1 == pthread_create(&pids[i], NULL, consumer, i)) {
            printf("error create thread\n");
            exit(1);
        }
    }

    for (int i = 0; i < 8; i++) {
        printf("\n#%d\n", i);

        pthread_barrier_wait(&cons_barrier);
        sleep(1);

        struct timeval start, prod_end;
        gettimeofday(&start, NULL);
        pthread_barrier_wait(&prod_barrier);
        pthread_barrier_wait(&prod_barrier);
        pthread_barrier_wait(&cons_barrier);
        gettimeofday(&prod_end, NULL);

        bool verify = true;
        for (int j = 1; j <= THREAD_NUM * COUNTS_PER_THREAD; ++j) {
            if (!array[j]) {
                printf("Error: ints[%d]\n", j);
                verify = false;
                break;
            }
        }
        if (verify)
            printf("ints[1-%ld] have been verified through\n",
                   THREAD_NUM * COUNTS_PER_THREAD);
        float cost_time = (prod_end.tv_sec - start.tv_sec) +
                          (prod_end.tv_usec - start.tv_usec) / 1000000.0;
        printf("elapsed time: %f seconds\n", cost_time);
        printf("DONE #%d\n", i);
        fflush(stdout);
        memset(array, 0, (1 + THREAD_NUM * COUNTS_PER_THREAD) * sizeof(bool));
    }
    return 0;
}
