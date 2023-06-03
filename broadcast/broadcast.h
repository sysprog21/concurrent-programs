#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Lock-free multi-publisher broadcast to multi-consumer */

typedef struct broadcast broadcast_t;
typedef struct broadcast_sub broadcast_sub_t;
struct __attribute__((aligned(16))) broadcast_sub {
    char _opaque[32];
};

broadcast_t *broadcast_new(size_t depth, size_t max_msg_size);
void broadcast_delete(broadcast_t *bcast);
bool broadcast_pub(broadcast_t *b, void *msg, size_t msg_size);
void broadcast_sub_begin(broadcast_sub_t *sub, broadcast_t *b);
bool broadcast_sub_next(broadcast_sub_t *sub,
                        void *msg_buf,
                        size_t *_out_msg_size,
                        size_t *_out_drops);

broadcast_t *broadcast_mem_init(void *mem, size_t depth, size_t max_msg_size);
