#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include "atomics.h"
#include "lfq.h"

#define MAX_FREE 150

static bool in_hp(struct lfq_ctx *ctx, struct lfq_node *node)
{
    for (int i = 0; i < ctx->MAX_HP_SIZE; i++) {
        if (atomic_load(&ctx->HP[i]) == node)
            return true;
    }
    return false;
}

/* add to tail of the free list */
static void insert_pool(struct lfq_ctx *ctx, struct lfq_node *node)
{
    atomic_store(&node->free_next, NULL);
    struct lfq_node *old_tail = XCHG(&ctx->fpt, node); /* seq_cst */
    atomic_store(&old_tail->free_next, node);
}

static void free_pool(struct lfq_ctx *ctx, bool freeall)
{
    bool old = 0;
    if (!CAS(&ctx->is_freeing, &old, 1))
        return;

    for (int i = 0; i < MAX_FREE || freeall; i++) {
        struct lfq_node *p = ctx->fph;
        if ((!atomic_load(&p->can_free)) || (!atomic_load(&p->free_next)) ||
            in_hp(ctx, (struct lfq_node *) p))
            break;
        ctx->fph = p->free_next;
        free(p);
    }
    atomic_store(&ctx->is_freeing, false);
    smp_mb();
}

static void safe_free(struct lfq_ctx *ctx, struct lfq_node *node)
{
    if (atomic_load(&node->can_free) && !in_hp(ctx, node)) {
        /* free is not thread-safe */
        bool old = 0;
        if (CAS(&ctx->is_freeing, &old, 1)) {
            /* poison the pointer to detect use-after-free */
            node->next = (void *) -1;
            free(node); /* we got the lock; actually free */
            atomic_store(&ctx->is_freeing, false);
            smp_mb();
        } else /* we did not get the lock; only add to a freelist */
            insert_pool(ctx, node);
    } else
        insert_pool(ctx, node);
    free_pool(ctx, false);
}

static int alloc_tid(struct lfq_ctx *ctx)
{
    for (int i = 0; i < ctx->MAX_HP_SIZE; i++) {
        if (ctx->tid_map[i] == 0) {
            int old = 0;
            if (CAS(&ctx->tid_map[i], &old, 1))
                return i;
        }
    }

    return -1;
}

static void free_tid(struct lfq_ctx *ctx, int tid)
{
    ctx->tid_map[tid] = 0;
}

int lfq_init(struct lfq_ctx *ctx, int max_consume_thread)
{
    struct lfq_node *tmp = calloc(1, sizeof(struct lfq_node));
    if (!tmp)
        return -errno;

    struct lfq_node *node = calloc(1, sizeof(struct lfq_node));
    if (!node)
        return -errno;

    tmp->can_free = node->can_free = true;
    memset(ctx, 0, sizeof(struct lfq_ctx));
    ctx->MAX_HP_SIZE = max_consume_thread;
    ctx->HP = calloc(max_consume_thread, sizeof(struct lfq_node));
    ctx->tid_map = calloc(max_consume_thread, sizeof(struct lfq_node));
    ctx->head = ctx->tail = tmp;
    ctx->fph = ctx->fpt = node;

    return 0;
}

long lfg_count_freelist(const struct lfq_ctx *ctx)
{
    long count = 0;
    for (struct lfq_node *p = (struct lfq_node *) ctx->fph; p; p = p->free_next)
        count++;
    return count;
}

int lfq_release(struct lfq_ctx *ctx)
{
    if (ctx->tail && ctx->head) {               /* if we have data in queue */
        while ((struct lfq_node *) ctx->head) { /* while still have node */
            struct lfq_node *tmp = (struct lfq_node *) ctx->head->next;
            safe_free(ctx, (struct lfq_node *) ctx->head);
            ctx->head = tmp;
        }
        ctx->tail = 0;
    }
    if (ctx->fph && ctx->fpt) {
        free_pool(ctx, true);
        if (ctx->fph != ctx->fpt)
            return -1;
        free(ctx->fpt); /* free the empty node */
        ctx->fph = ctx->fpt = 0;
    }
    if (ctx->fph || ctx->fpt)
        return -1;

    free(ctx->HP);
    free(ctx->tid_map);
    memset(ctx, 0, sizeof(struct lfq_ctx));

    return 0;
}

int lfq_enqueue(struct lfq_ctx *ctx, void *data)
{
    struct lfq_node *insert_node = calloc(1, sizeof(struct lfq_node));
    if (!insert_node)
        return -errno;

    insert_node->data = data;
    struct lfq_node *old_tail = XCHG(&ctx->tail, insert_node);
    /* We have claimed our spot in the insertion order by modifying tail.
     * we are the only inserting thread with a pointer to the old tail.
     *
     * Now we can make it part of the list by overwriting the NULL pointer in
     * the old tail. This is safe whether or not other threads have updated
     * ->next in our insert_node.
     */
    assert(!old_tail->next && "old tail was not NULL");
    atomic_store(&old_tail->next, insert_node);

    return 0;
}

void *lfq_dequeue_tid(struct lfq_ctx *ctx, int tid)
{
    struct lfq_node *old_head, *new_head;

    /* HP[tid] is necessary for deallocation. */
    do {
    retry:
        /* continue jumps to the bottom of the loop, and would attempt a CAS
         * with uninitialized new_head.
         */
        old_head = atomic_load(&ctx->head);

        atomic_store(&ctx->HP[tid], old_head);
        mb();

        /* another thread freed it before seeing our HP[tid] store */
        if (old_head != atomic_load(&ctx->head))
            goto retry;
        new_head = atomic_load(&old_head->next);

        if (new_head == 0) {
            atomic_store(&ctx->HP[tid], 0);
            return NULL; /* never remove the last node */
        }
    } while (!CAS(&ctx->head, &old_head, new_head));

    /* We have atomically advanced head, and we are the thread that won the race
     * to claim a node. We return the data from the *new* head. The list starts
     * off with a dummy node, so the current head is always a node that is
     * already been read.
     */
    atomic_store(&ctx->HP[tid], 0);
    void *ret = new_head->data;
    atomic_store(&new_head->can_free, true);

    /* we need to avoid freeing until other readers are definitely not going to
     * load its ->next in the CAS loop
     */
    safe_free(ctx, (struct lfq_node *) old_head);

    return ret;
}

void *lfq_dequeue(struct lfq_ctx *ctx)
{
    int tid = alloc_tid(ctx);
    /* To many thread race */
    if (tid == -1)
        return (void *) -1;

    void *ret = lfq_dequeue_tid(ctx, tid);
    free_tid(ctx, tid);
    return ret;
}