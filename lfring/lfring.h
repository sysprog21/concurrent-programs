/* Lock-free multiple-producer (MP) /multiple-consumer (MC) ring buffer. */

#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    LFRING_FLAG_MP = 0x0000 /* Multiple producer */,
    LFRING_FLAG_SP = 0x0001 /* Single producer */,
    LFRING_FLAG_MC = 0x0000 /* Multi consumer */,
    LFRING_FLAG_SC = 0x0002 /* Single consumer */,
};

typedef struct lfring lfring_t;

/* Allocate ring buffer with space for at least 'n_elems' elements.
 * 'n_elems' != 0 and 'n_elems' <= 0x80000000
 */
lfring_t *lfring_alloc(uint32_t n_elems, uint32_t flags);

/* Free ring buffer.
 * The ring buffer must be empty
 */
void lfring_free(lfring_t *lfr);

/* Enqueue elements on ring buffer.
 * The number of actually enqueued elements is returned.
 */
uint32_t lfring_enqueue(lfring_t *lfr, void *const elems[], uint32_t n_elems);

/* Dequeue elements from ring buffer.
 * The number of actually dequeued elements is returned.
 */
uint32_t lfring_dequeue(lfring_t *lfr,
                        void *elems[],
                        uint32_t n_elems,
                        uint32_t *index);
