#pragma once

#include <stdatomic.h>
#include <stdint.h>

typedef struct mcsnode {
    _Atomic(struct mcsnode *) next;
    _Atomic(uint8_t) wait;
} mcsnode_t;

typedef _Atomic(mcsnode_t *) mcslock_t;

/* Initialize an MCS lock */
void mcslock_init(mcslock_t *lock);

/* Acquire an MCS lock
 * 'node' points to an uninitialized mcsnode
 */
void mcslock_acquire(mcslock_t *lock, mcsnode_t *node);

/* Release an MCS lock
 * node' must specify same node as used in matching acquire call
 */
void mcslock_release(mcslock_t *lock, mcsnode_t *node);
