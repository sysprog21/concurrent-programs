#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct list_node {
    struct list_node *next;
    void *val;
} list_node_t;

typedef struct {
    list_node_t *head;
    uint32_t length;
} list_t;

static volatile uint32_t list_retries_empty = 0, list_retries_populated = 0;
static const list_node_t *empty = NULL;

static list_t *list_new()
{
    list_t *l = calloc(1, sizeof(list_node_t));
    l->head = (list_node_t *) empty;
    l->length = 0;
    return l;
}

static void list_add(list_t *l, void *val)
{
    /* wrap the value as a node in the linked list */
    list_node_t *v = calloc(1, sizeof(list_node_t));
    v->val = val;

    /* try adding to the front of the list */
    while (true) {
        list_node_t *n = l->head;
        if (n == empty) { /* if this is the first link in the list */
            v->next = NULL;
            if (__atomic_compare_exchange(&l->head, &empty, &v, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                __atomic_fetch_add(&l->length, 1, __ATOMIC_SEQ_CST);
                return;
            }
            list_retries_empty++;
        } else { /* inserting when an existing link is present */
            v->next = n;
            if (__atomic_compare_exchange(&l->head, &n, &v, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                __atomic_fetch_add(&l->length, 1, __ATOMIC_SEQ_CST);
                return;
            }
            list_retries_populated++;
        }
    }
}

#include "free_later.h"

#define CAS(a, b, c)                                                    \
    __extension__({                                                     \
        typeof(*a) _old = b, _new = c;                                  \
        __atomic_compare_exchange(a, &_old, &_new, 0, __ATOMIC_SEQ_CST, \
                                  __ATOMIC_SEQ_CST);                    \
        _old;                                                           \
    })

static inline void acquire_lock(volatile bool *lock)
{
    while (CAS(lock, false, true))
        ;
}

static inline void release_lock(volatile bool *lock)
{
    int l = *lock;
    CAS(&l, true, false);
}

typedef struct {
    void *var;
    void (*free)(void *var);
} free_later_t;

/* track expired variables to cleanup later */
static list_t *buffer = NULL, *buffer_prev = NULL;

int free_later_init()
{
    buffer = list_new();
    return 0;
}

/* register a var for cleanup */
void free_later(void *var, void release(void *var))
{
    free_later_t *cv = malloc(sizeof(free_later_t));
    cv->var = var;
    cv->free = release;
    list_add(buffer, cv);
}

/* signal that worker threads are done with old references */
void free_later_stage(void)
{
    /* lock to ensure that multiple threads do not clean up simultaneously */
    static bool lock = false;

    /* CAS-based lock in case multiple threads are calling this */
    acquire_lock(&lock);

    if (!buffer_prev || buffer_prev->length == 0) {
        release_lock(&lock);
        return;
    }

    /* swap the buffers */
    buffer_prev = buffer;
    buffer = list_new();

    release_lock(&lock);
}

void free_later_run()
{
    /* lock to ensure that multiple threads do not clean up simultaneously */
    static bool lock = false;

    /* skip if there is nothing to return */
    if (!buffer_prev)
        return;

    /* CAS-based lock in case multiple threads are calling this */
    acquire_lock(&lock);

    /* At this point, all workers have processed one or more new flow since the
     * free_later buffer was filled. No threads are using the old, deleted data.
     */
    for (list_node_t *n = buffer_prev->head; n; n = n->next) {
        free_later_t *v = n->val;
        v->free(v->var);
        free(n);
    }

    free(buffer_prev);
    buffer_prev = NULL;

    release_lock(&lock);
}

int free_later_exit()
{
    /* purge anything that is buffered */
    free_later_run();

    /* stage and purge anything that was unbuffered */
    free_later_stage();
    free_later_run();

    /* release memory for the buffer */
    free(buffer);
    buffer = NULL;
    return 0;
}
