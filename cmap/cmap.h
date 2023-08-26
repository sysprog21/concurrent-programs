#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rcu.h"
#include "util.h"

/* Concurrent cmap.
 * It supports multiple concurrent readers and a single concurrent writer.
 * To iterate, the user needs to acquire a "cmap state" (snapshot).
 */
struct cmap_node {
    struct cmap_node *next; /* Next node with same hash. */
    uint32_t hash;
};

/* Used for going over all cmap nodes */
struct cmap_cursor {
    struct cmap_node *node; /* Pointer to cmap_node */
    struct cmap_node *next; /* Pointer to cmap_node */
    size_t entry_idx;       /* Current entry */
    bool accross_entries;   /* Hold cursor accross cmap entries */
};

/* Map state (snapshot), must be acquired before cmap iteration, and released
 * afterwards.
 */
struct cmap_state {
    struct rcu *p;
};

/* Concurrent hash cmap. */
struct cmap {
    struct cmap_state *impl;
};

/* Initialization. */
void cmap_init(struct cmap *);
void cmap_destroy(struct cmap *);

/* Counters. */
size_t cmap_size(const struct cmap *);
double cmap_utilization(const struct cmap *cmap);

/* Insertion and deletion. Return the current count after the operation. */
size_t cmap_insert(struct cmap *, struct cmap_node *, uint32_t hash);
size_t cmap_remove(struct cmap *, struct cmap_node *);

/* Acquire/release cmap concurrent state. Use with iteration macros.
 * Each acquired state must be released. */
struct cmap_state cmap_state_acquire(struct cmap *cmap);
void cmap_state_release(struct cmap_state state);

/* Iteration macros. Usage example:
 *
 * struct {
 *     struct cmap_node node;
 *     int value;
 * } *data;
 * struct cmap_state *cmap_state = cmap_state_acquire(&cmap);
 * MAP_FOREACH(data, node, cmap_state) {
 *      ...
 * }
 * cmap_state_release(cmap_state);
 */
#define MAP_FOREACH(NODE, MEMBER, STATE) \
    MAP_FOREACH__(NODE, MEMBER, MAP, cmap_start__(STATE), STATE)

#define MAP_FOREACH_WITH_HASH(NODE, MEMBER, HASH, STATE) \
    MAP_FOREACH__(NODE, MEMBER, MAP, cmap_find__(STATE, HASH), STATE)

/* Ieration, private methods. Use iteration macros instead */
struct cmap_cursor cmap_start__(struct cmap_state state);
struct cmap_cursor cmap_find__(struct cmap_state state, uint32_t hash);
void cmap_next__(struct cmap_state state, struct cmap_cursor *cursor);

#define MAP_FOREACH__(NODE, MEMBER, MAP, START, STATE)                      \
    for (struct cmap_cursor cursor_ = START;                                \
         (cursor_.node ? (INIT_CONTAINER(NODE, cursor_.node, MEMBER), true) \
                       : false);                                            \
         cmap_next__(STATE, &cursor_))
