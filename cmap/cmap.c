#include "cmap.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "locks.h"
#include "util.h"

#define MAP_INITIAL_SIZE 512

struct cmap_entry {
    struct cmap_node *first;
};

struct cmap_impl {
    struct cmap_entry *arr; /* Map entreis */
    size_t count;           /* Number of elements in this */
    size_t max;             /* Capacity of this */
    size_t utilization;     /* Number of utialized entries */
    struct cond fence;      /* Prevent new reads while old still exist */
};

struct cmap_impl_pair {
    struct cmap_impl *old, *new;
};

static void cmap_expand(struct cmap *cmap);
static void cmap_expand_callback(void *args);
static void cmap_destroy_callback(void *args);
static size_t cmap_count__(const struct cmap *cmap);
static void cmap_insert__(struct cmap_impl *, struct cmap_node *);

/* Only a single concurrent writer to cmap is allowed */
static void cmap_insert__(struct cmap_impl *impl, struct cmap_node *node)
{
    size_t i = node->hash & impl->max;
    node->next = impl->arr[i].first;
    if (!impl->arr[i].first)
        impl->utilization++;
    impl->arr[i].first = node;
}

static void cmap_destroy_callback(void *args)
{
    struct cmap_impl *impl = (struct cmap_impl *) args;
    cond_destroy(&impl->fence);
    free(impl);
}

static struct cmap_impl *cmap_impl_init(size_t entry_num)
{
    size_t size =
        sizeof(struct cmap_impl) + sizeof(struct cmap_entry) * entry_num;
    struct cmap_impl *impl = xmalloc(size);
    impl->max = entry_num - 1;
    impl->count = 0;
    impl->utilization = 0;
    impl->arr = OBJECT_END(struct cmap_entry *, impl);
    cond_init(&impl->fence);

    for (int i = 0; i < entry_num; ++i)
        impl->arr[i].first = NULL;
    return impl;
}

static void cmap_expand_callback(void *args)
{
    struct cmap_impl_pair *pair = (struct cmap_impl_pair *) args;
    struct cmap_node *c, *n;

    /* Rehash */
    for (int i = 0; i <= pair->old->max; i++) {
        for (c = pair->old->arr[i].first; c; c = n) {
            n = c->next;
            cmap_insert__(pair->new, c);
        }
    }

    /* Remove fence */
    cond_unlock(&pair->new->fence);
    free(pair->old);
    free(pair);
}

/* Only a single concurrent writer to cmap is allowed */
static void cmap_expand(struct cmap *cmap)
{
    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl_pair *pair = xmalloc(sizeof(*pair));
    pair->old = rcu_get(impl_rcu, struct cmap_impl *);

    /* Do not allow two expansions in parallel */
    /* Prevent new reads while old still exist */
    while (cond_is_locked(&pair->old->fence)) {
        rcu_release(impl_rcu);
        cond_wait(&pair->old->fence);
        impl_rcu = rcu_acquire(cmap->impl->p);
        pair->old = rcu_get(impl_rcu, struct cmap_impl *);
    }

    /* Initiate new rehash array */
    pair->new = cmap_impl_init((pair->old->max + 1) * 2);
    pair->new->count = pair->old->count;

    /* Prevent new reads/updates while old reads still exist */
    cond_lock(&pair->new->fence);

    rcu_postpone(impl_rcu, cmap_expand_callback, pair);
    rcu_release(impl_rcu);
    rcu_set(cmap->impl->p, pair->new);
}

void cmap_init(struct cmap *cmap)
{
    struct cmap_impl *impl = cmap_impl_init(MAP_INITIAL_SIZE);
    cmap->impl = xmalloc(sizeof(*cmap->impl));
    rcu_init(cmap->impl->p, impl);
}

void cmap_destroy(struct cmap *cmap)
{
    if (!cmap)
        return;

    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl *impl = rcu_get(impl_rcu, struct cmap_impl *);
    rcu_postpone(impl_rcu, cmap_destroy_callback, impl);
    rcu_release(impl_rcu);
    rcu_destroy(impl_rcu);
    free(cmap->impl);
}

static size_t cmap_count__(const struct cmap *cmap)
{
    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl *impl = rcu_get(impl_rcu, struct cmap_impl *);
    size_t count = impl->count;
    rcu_release(impl_rcu);
    return count;
}

double cmap_utilization(const struct cmap *cmap)
{
    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl *impl = rcu_get(impl_rcu, struct cmap_impl *);
    double res = (double) impl->utilization / (impl->max + 1);
    rcu_release(impl_rcu);
    return res;
}

size_t cmap_size(const struct cmap *cmap)
{
    return cmap_count__(cmap);
}

/* Only one concurrent writer */
size_t cmap_insert(struct cmap *cmap, struct cmap_node *node, uint32_t hash)
{
    node->hash = hash;

    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl *impl = rcu_get(impl_rcu, struct cmap_impl *);
    cmap_insert__(impl, node);
    impl->count++;
    size_t count = impl->count;
    bool expand = impl->count > impl->max * 2;
    rcu_release(impl_rcu);

    if (expand)
        cmap_expand(cmap);
    return count;
}

/* Only one concurrent writer */
size_t cmap_remove(struct cmap *cmap, struct cmap_node *node)
{
    struct rcu *impl_rcu = rcu_acquire(cmap->impl->p);
    struct cmap_impl *impl = rcu_get(impl_rcu, struct cmap_impl *);
    size_t pos = node->hash & impl->max;
    struct cmap_entry *cmap_entry = &impl->arr[pos];
    size_t count = impl->count;

    struct cmap_node **node_p = &cmap_entry->first;
    while (*node_p) {
        if (*node_p == node) {
            *node_p = node->next;
            count--;
            break;
        }
        node_p = &(*node_p)->next;
    }
    impl->count = count;
    rcu_release(impl_rcu);
    return count;
}

struct cmap_state cmap_state_acquire(struct cmap *cmap)
{
    struct cmap_state state = {.p = rcu_acquire(cmap->impl->p)};
    return state;
}

void cmap_state_release(struct cmap_state state)
{
    rcu_release(state.p);
}

struct cmap_cursor cmap_find__(struct cmap_state state, uint32_t hash)
{
    struct cmap_impl *impl = rcu_get(state.p, struct cmap_impl *);

    /* Prevent new reads while old still exist */
    while (cond_is_locked(&impl->fence))
        cond_wait(&impl->fence);

    struct cmap_cursor cursor = {
        .entry_idx = hash & impl->max,
        .node = impl->arr[hash & impl->max].first,
        .next = NULL,
        .accross_entries = false,
    };
    if (cursor.node)
        cursor.next = cursor.node->next;
    return cursor;
}

struct cmap_cursor cmap_start__(struct cmap_state state)
{
    struct cmap_cursor cursor = cmap_find__(state, 0);
    cursor.accross_entries = true;
    /* Don't start with an empty node */
    if (!cursor.node)
        cmap_next__(state, &cursor);
    return cursor;
}

void cmap_next__(struct cmap_state state, struct cmap_cursor *cursor)
{
    struct cmap_impl *impl = rcu_get(state.p, struct cmap_impl *);

    cursor->node = cursor->next;
    if (cursor->node) {
        cursor->next = cursor->node->next;
        return;
    }

    /* We got to the end of the current entry. Try to find
     * a valid node in next entries
     */
    while (cursor->accross_entries) {
        cursor->entry_idx++;
        if (cursor->entry_idx > impl->max)
            break;
        cursor->node = impl->arr[cursor->entry_idx].first;
        if (cursor->node) {
            cursor->next = cursor->node->next;
            return;
        }
    }

    cursor->node = NULL;
    cursor->next = NULL;
}
