#include <stdlib.h>

#include "broadcast.h"
#include "pool.h"
#include "util.h"

#define ESTIMATED_PUBLISHERS 16

struct __attribute__((aligned(CACHELINE_SIZE))) broadcast {
    uint64_t depth_mask;
    uint64_t max_msg_size;
    uint64_t head_idx, tail_idx;
    size_t pool_off;
    char padding[CACHELINE_SIZE - 5 * sizeof(uint64_t) - sizeof(size_t)];

    lf_ref_t slots[];
};
static_assert(sizeof(broadcast_t) == CACHELINE_SIZE, "");
static_assert(alignof(broadcast_t) == CACHELINE_SIZE, "");

typedef struct __attribute__((aligned(alignof(uint128_t)))) msg {
    uint64_t size;
    uint8_t payload[];
} msg_t;

static inline pool_t *get_pool(broadcast_t *b)
{
    return (pool_t *) ((char *) b + b->pool_off);
}

static void broadcast_footprint(size_t depth,
                                size_t max_msg_size,
                                size_t *size,
                                size_t *align);

broadcast_t *broadcast_new(size_t depth, size_t max_msg_size)
{
    size_t mem_size, mem_align;
    broadcast_footprint(depth, max_msg_size, &mem_size, &mem_align);

    void *mem = NULL;
    int ret = posix_memalign(&mem, mem_align, mem_size);
    if (ret != 0)
        return NULL;

    broadcast_t *bcast = broadcast_mem_init(mem, depth, max_msg_size);
    if (!bcast) {
        free(mem);
        return NULL;
    }

    return bcast;
}

void broadcast_delete(broadcast_t *bcast)
{
    free(bcast);
}

static void try_drop_head(broadcast_t *b, uint64_t head_idx)
{
    lf_ref_t head_cur = b->slots[head_idx & b->depth_mask];
    if (!LF_U64_CAS(&b->head_idx, head_idx, head_idx + 1))
        return;

    /* TODO: release the shared resources */
    uint64_t msg_off = head_cur.val;
    msg_t *msg = (msg_t *) ((char *) b + msg_off);
    pool_release(get_pool(b), msg);
}

bool broadcast_pub(broadcast_t *b, void *msg_buf, size_t msg_size)
{
    msg_t *msg = (msg_t *) pool_acquire(get_pool(b));
    if (!msg)
        return false; /* out of elements */
    uint64_t msg_off = (char *) msg - (char *) b;

    msg->size = msg_size;
    memcpy(msg->payload, msg_buf, msg_size);

    while (1) {
        uint64_t head_idx = b->head_idx;
        uint64_t tail_idx = b->tail_idx;
        lf_ref_t *tail_ptr = &b->slots[tail_idx & b->depth_mask];
        lf_ref_t tail_cur = *tail_ptr;
        LF_BARRIER_ACQUIRE();

        /* Stale tail pointer? Try to advance it */
        if (tail_cur.tag == tail_idx) {
            LF_U64_CAS(&b->tail_idx, tail_idx, tail_idx + 1);
            LF_PAUSE();
            continue;
        }

        /* Stale tail_idx? Try again */
        if (tail_cur.tag >= tail_idx) {
            LF_PAUSE();
            continue;
        }

        /* Slot currently used. if full, roll off the head */
        if (head_idx <= tail_cur.tag) {
            try_drop_head(b, head_idx);
            LF_PAUSE();
            continue;
        }

        /* Otherwise, try to append the tail */
        lf_ref_t tail_next = LF_REF_MAKE(tail_idx, msg_off);
        if (!LF_REF_CAS(tail_ptr, tail_cur, tail_next)) {
            LF_PAUSE();
            continue;
        }

        /* Success, try to update the tail. */
        LF_U64_CAS(&b->tail_idx, tail_idx, tail_idx + 1);
        return true;
    }
}

typedef struct sub_impl sub_impl_t;
struct __attribute__((aligned(16))) sub_impl {
    broadcast_t *bcast;
    uint64_t idx;
    char _extra[16];
};
static_assert(sizeof(sub_impl_t) == sizeof(broadcast_sub_t), "");
static_assert(alignof(sub_impl_t) == alignof(broadcast_sub_t), "");

void broadcast_sub_begin(broadcast_sub_t *_sub, broadcast_t *b)
{
    sub_impl_t *sub = (sub_impl_t *) _sub;
    sub->bcast = b;
    sub->idx = b->head_idx;
}

bool broadcast_sub_next(broadcast_sub_t *_sub,
                        void *msg_buf,
                        size_t *_out_msg_size,
                        size_t *_out_drops)
{
    sub_impl_t *sub = (sub_impl_t *) _sub;
    broadcast_t *b = sub->bcast;
    size_t drops = 0;

    while (1) {
        if (sub->idx == b->tail_idx)
            return false;

        lf_ref_t *ref_ptr = &b->slots[sub->idx & b->depth_mask];
        lf_ref_t ref = *ref_ptr;

        LF_BARRIER_ACQUIRE();

        /* we have fallen behind and the message we wanted was dropped? */
        if (ref.tag != sub->idx) {
            sub->idx++;
            drops++;
            LF_PAUSE();
            continue;
        }
        uint64_t msg_off = ref.val;
        msg_t *msg = (msg_t *) ((char *) b + msg_off);
        size_t msg_size = msg->size;
        if (msg_size > b->max_msg_size) { /* inconsistent */
            LF_PAUSE();
            continue;
        }
        memcpy(msg_buf, msg->payload, msg_size);

        LF_BARRIER_ACQUIRE();

        lf_ref_t ref2 = *ref_ptr;
        /* Data hanged while reading? Drop it */
        if (!LF_REF_EQUAL(ref, ref2)) {
            sub->idx++;
            drops++;
            LF_PAUSE();
            continue;
        }

        sub->idx++;
        *_out_msg_size = msg_size;
        *_out_drops = drops;
        return true;
    }
}

static void broadcast_footprint(size_t depth,
                                size_t max_msg_size,
                                size_t *_size,
                                size_t *_align)
{
    size_t elt_size =
        LF_ALIGN_UP(sizeof(msg_t) + max_msg_size, alignof(uint128_t));
    size_t pool_elts = depth + ESTIMATED_PUBLISHERS;

    size_t pool_size, pool_align;
    pool_footprint(pool_elts, elt_size, &pool_size, &pool_align);

    size_t size = sizeof(broadcast_t);
    size += depth * sizeof(lf_ref_t);
    size = LF_ALIGN_UP(size, pool_align);
    size += pool_size;

    if (_size)
        *_size = size;
    if (_align)
        *_align = alignof(broadcast_t);
}

broadcast_t *broadcast_mem_init(void *mem, size_t depth, size_t max_msg_size)
{
    if (!LF_IS_POW2(depth))
        return NULL;
    if (max_msg_size == 0)
        return NULL;

    size_t elt_size =
        LF_ALIGN_UP(sizeof(msg_t) + max_msg_size, alignof(uint128_t));
    size_t pool_elts = depth + ESTIMATED_PUBLISHERS;

    size_t pool_size, pool_align;
    pool_footprint(pool_elts, elt_size, &pool_size, &pool_align);

    size_t size = sizeof(broadcast_t) + depth * sizeof(lf_ref_t);
    size_t pool_off = LF_ALIGN_UP(size, pool_align);
    void *pool_mem = (char *) mem + pool_off;

    broadcast_t *b = (broadcast_t *) mem;
    b->depth_mask = depth - 1;
    b->max_msg_size = max_msg_size;
    /* Start from 1 because we use 0 to mean "unused" */
    b->head_idx = 1, b->tail_idx = 1;
    b->pool_off = pool_off;

    memset(b->slots, 0, depth * sizeof(lf_ref_t));

    pool_t *pool = pool_mem_init(pool_mem, pool_elts, elt_size);
    if (!pool)
        return NULL;
    assert(pool == pool_mem);

    return b;
}
