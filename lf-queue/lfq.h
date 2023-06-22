#pragma once

#include <stdalign.h>
#include <stdbool.h>

struct lfq_node {
    void *data;
    union {
        struct lfq_node *next;
        struct lfq_node *free_next;
    };
    bool can_free;
};

struct lfq_ctx {
    alignas(64) struct lfq_node *head;
    int count;
    struct lfq_node **HP; /* hazard pointers */
    int *tid_map;
    bool is_freeing;
    struct lfq_node *fph, *fpt; /* free pool head/tail */

    /* FIXME: get rid of struct. Make it configurable */
    int MAX_HP_SIZE;

    /* avoid cacheline contention */
    alignas(64) struct lfq_node *tail;
};

/**
 * lfq_init - Initialize lock-free queue.
 * @ctx: Lock-free queue handler.
 * @max_consume_thread: Max consume thread numbers. If this value set to zero,
 *                      use default value (16).
 * Return zero on success. On error, negative errno.
 */
int lfq_init(struct lfq_ctx *ctx, int max_consume_thread);

/**
 * lfq_release - Release lock-free queue from ctx.
 * @ctx: Lock-free queue handler.
 * Return zero on success. On error, -1.
 */
int lfq_release(struct lfq_ctx *ctx);

/* internal function */
long lfg_count_freelist(const struct lfq_ctx *ctx);

/**
 * lfq_enqueue - Push data into queue.
 * @ctx: Lock-free queue handler.
 * @data: User data
 * Return zero on success. On error, negative errno.
 */
int lfq_enqueue(struct lfq_ctx *ctx, void *data);

/**
 * lfq_dequeue_tid - Pop data from queue.
 * @ctx: Lock-free queue handler.
 * @tid: Unique thread id.
 * Return zero if empty queue. On error, negative errno.
 */
void *lfq_dequeue_tid(struct lfq_ctx *ctx, int tid);

/**
 * lfq_dequeue - Pop data from queue.
 * @ctx: Lock-free queue handler.
 */
void *lfq_dequeue(struct lfq_ctx *ctx);