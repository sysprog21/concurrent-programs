#include "pool.h"
#include "util.h"

struct __attribute__((aligned(CACHELINE_SIZE))) pool {
    size_t num_elts;
    size_t elt_size;
    uint64_t tag_next;
    uint64_t _pad1;
    lf_ref_t head;
    char _pad2[CACHELINE_SIZE - 2 * sizeof(size_t) - 2 * sizeof(uint64_t) -
               sizeof(lf_ref_t)];

    char mem[];
};
static_assert(sizeof(pool_t) == CACHELINE_SIZE, "");
static_assert(alignof(pool_t) == CACHELINE_SIZE, "");

void *pool_acquire(pool_t *pool)
{
    while (1) {
        lf_ref_t cur = pool->head;
        if (LF_REF_IS_NULL(cur))
            return NULL;

        uint64_t elt_off = cur.val;
        lf_ref_t *elt = (lf_ref_t *) ((char *) pool + elt_off);
        lf_ref_t next = *elt;

        if (!LF_REF_CAS(&pool->head, cur, next)) {
            LF_PAUSE();
            continue;
        }
        return elt;
    }
}

void pool_release(pool_t *pool, void *elt)
{
    uint64_t elt_off = (uint64_t) ((char *) elt - (char *) pool);
    uint64_t tag = LF_ATOMIC_INC(&pool->tag_next);
    lf_ref_t next = LF_REF_MAKE(tag, elt_off);

    while (1) {
        lf_ref_t cur = pool->head;
        *(lf_ref_t *) elt = cur;

        if (!LF_REF_CAS(&pool->head, cur, next)) {
            LF_PAUSE();
            continue;
        }
        return;
    }
}

void pool_footprint(size_t num_elts,
                    size_t elt_size,
                    size_t *_size,
                    size_t *_align)
{
    elt_size = LF_ALIGN_UP(elt_size, alignof(uint128_t));

    if (_size)
        *_size = sizeof(pool_t) + elt_size * num_elts;
    if (_align)
        *_align = alignof(pool_t);
}

pool_t *pool_mem_init(void *mem, size_t num_elts, size_t elt_size)
{
    if (elt_size == 0)
        return NULL;
    elt_size = LF_ALIGN_UP(elt_size, alignof(uint128_t));

    pool_t *pool = (pool_t *) mem;
    pool->num_elts = num_elts;
    pool->elt_size = elt_size;
    pool->tag_next = 0;
    pool->head = LF_REF_NULL;

    char *ptr = pool->mem + num_elts * elt_size;
    for (size_t i = num_elts; i > 0; i--) {
        ptr -= elt_size;
        pool_release(pool, ptr);
    }

    return pool;
}
