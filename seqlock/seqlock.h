#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t seqlock_t;

/* Initialise a seqlock aka reader/writer synchronization */
void seqlock_init(seqlock_t *sync);

/* Acquire a seqlock for reading
 * Block until no write is in progress
 */
seqlock_t seqlock_acquire_rd(const seqlock_t *sync);

/* Release a read seqlock
 * Return false if a write has occurred or is in progress
 * This means any read data may be inconsistent and the operation should be
 * restarted
 */
bool seqlock_release_rd(const seqlock_t *sync, seqlock_t prv);

/* Acquire a seqlock for writing
 * Block until earlier writes have completed
 */
void seqlock_acquire_wr(seqlock_t *sync);

/* Release a write seqlock */
void seqlock_release_wr(seqlock_t *sync);

/* Perform an atomic read of the associated data
 * Will block for concurrent writes
 */
void seqlock_read(seqlock_t *sync, void *dst, const void *data, size_t len);

/* Perform an atomic write of the associated data
 * Will block for concurrent writes
 */
void seqlock_write(seqlock_t *sync, const void *src, void *data, size_t len);
