#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lfq.h"

#ifndef MAX_PRODUCER
#define MAX_PRODUCER 100
#endif
#ifndef MAX_CONSUMER
#define MAX_CONSUMER 10
#endif

#define SOME_ID 667814649

static uint64_t cnt_added = 0;
static uint64_t cnt_removed = 0;

static int cnt_thread = 0;
static int cnt_producer = 0;

struct user_data {
    long data;
};

void *add_queue(void *data)
{
    struct lfq_ctx *ctx = data;
    long added;
    for (added = 0; added < 500000; added++) {
        struct user_data *p = malloc(sizeof(struct user_data));
        p->data = SOME_ID;
        int ret = 0;
        if ((ret = lfq_enqueue(ctx, p)) != 0) {
            printf("lfq_enqueue failed, reason:%s\n", strerror(-ret));
            atomic_fetch_add(&cnt_added, added);
            atomic_fetch_sub(&cnt_producer, 1);
            return 0;
        }
    }
    atomic_fetch_add(&cnt_added, added);
    atomic_fetch_sub(&cnt_producer, 1);
    printf("Producer thread [%lu] exited! Still %d running...\n",
           pthread_self(), atomic_load(&cnt_producer));
    return 0;
}

void *remove_queue(void *data)
{
    struct lfq_ctx *ctx = data;
    int tid = atomic_fetch_add(&cnt_thread, 1);
    long deleted = 0;
    while (1) {
        struct user_data *p = lfq_dequeue_tid(ctx, tid);
        if (p) {
            if (p->data != SOME_ID) {
                printf("data wrong!!\n");
                exit(1);
            }

            free(p);
            deleted++;
        } else {
            if (!ctx->count && !atomic_load(&cnt_producer))
                break;     /* queue is empty and no more producers */
            sched_yield(); /* queue is empty, release CPU slice */
        }
    }
    atomic_fetch_add(&cnt_removed, deleted);

    printf("Consumer thread [%lu] exited %d\n", pthread_self(), cnt_producer);
    return 0;
}

int main()
{
    struct lfq_ctx ctx;
    lfq_init(&ctx, MAX_CONSUMER);

    pthread_t thread_cons[MAX_CONSUMER], thread_pros[MAX_PRODUCER];

    atomic_fetch_add(&cnt_producer, 1);
    for (int i = 0; i < MAX_CONSUMER; i++) {
        pthread_create(&thread_cons[i], NULL, remove_queue, (void *) &ctx);
    }

    for (int i = 0; i < MAX_PRODUCER; i++) {
        atomic_fetch_add(&cnt_producer, 1);
        pthread_create(&thread_pros[i], NULL, add_queue, (void *) &ctx);
    }

    atomic_fetch_sub(&cnt_producer, 1);

    for (int i = 0; i < MAX_PRODUCER; i++)
        pthread_join(thread_pros[i], NULL);

    for (int i = 0; i < MAX_CONSUMER; i++)
        pthread_join(thread_cons[i], NULL);

    long cnt_free = lfg_count_freelist(&ctx);
    int clean = lfq_release(&ctx);
    printf("Total push %" PRId64 " elements, pop %" PRId64
           " elements. freelist=%ld, clean = %d\n",
           cnt_added, cnt_removed, cnt_free, clean);

    if (cnt_added == cnt_removed)
        printf("Test PASS!!\n");
    else
        printf("Test Failed!!\n");

    return (cnt_added != cnt_removed);
}
