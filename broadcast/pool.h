#pragma once

#include <stddef.h>

/* Lock-free Pool */

typedef struct pool pool_t;

void pool_delete(pool_t *pool);
void *pool_acquire(pool_t *pool);
void pool_release(pool_t *pool, void *elt);

void pool_footprint(size_t num_elts,
                    size_t elt_size,
                    size_t *size,
                    size_t *align);
pool_t *pool_mem_init(void *mem, size_t num_elts, size_t elt_size);
