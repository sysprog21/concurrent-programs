/* A concurrent linked list utilizing the simplified RCU algorithm */

#include <stdbool.h>

typedef struct rcu_list rcu_list_t;

typedef struct {
    struct list_node *entry;
} iterator_t;

typedef struct {
    struct rcu_list *list;
    struct zombie_node *zombie;
} rcu_handle_t;

typedef rcu_handle_t read_handle_t;
typedef rcu_handle_t write_handle_t;

typedef void (*deleter_func_t)(void *);
typedef bool (*finder_func_t)(void *, void *);

#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct list_node {
    bool deleted;
    struct list_node *next, *prev;
    void *data;
} list_node_t;

typedef struct zombie_node {
    struct zombie_node *next;
    struct list_node *zombie;
    rcu_handle_t *owner;
} zombie_node_t;

struct rcu_list {
    pthread_mutex_t write_lock; /* exclusive lock acquired by writers */
    list_node_t *head, *tail;   /* head and tail of the "live" list */
    zombie_node_t *zombie_head; /* head of the zombie list */
    deleter_func_t deleter;
};

static list_node_t *make_node(void *data);

static zombie_node_t *make_zombie_node(void);

static void lock_for_write(rcu_list_t *list);
static void unlock_for_write(rcu_list_t *list);

static rcu_list_t *list_new_with_deleter(deleter_func_t deleter)
{
    if (!deleter)
        return NULL;

    rcu_list_t *list = malloc(sizeof(rcu_list_t));
    if (!list)
        return NULL;

    if (pthread_mutex_init(&list->write_lock, NULL) != 0) {
        free(list);
        return NULL;
    }

    list->head = list->tail = NULL;
    list->zombie_head = NULL;
    list->deleter = deleter;

    return list;
}

static void list_free(void *arg)
{
    rcu_list_t *list = arg;
    for (list_node_t *iter = list->head; iter;) {
        list_node_t *tmp = iter->next;
        free(iter->data);
        free(iter);
        iter = tmp;
    }
    free(list);
}

rcu_list_t *list_new(void)
{
    return list_new_with_deleter(list_free);
}

void list_delete(rcu_list_t *list)
{
    if (!list || !list->deleter)
        return;
    list->deleter(list);
}

void list_push_front(rcu_list_t *list, void *data, write_handle_t *handle)
{
    if (!list)
        return;

    list_node_t *node = make_node(data);
    if (!node)
        return;

    lock_for_write(list);

    list_node_t *old_head;
    __atomic_load(&list->head, &old_head, __ATOMIC_RELAXED);

    if (!old_head) {
        /* list is currently empty */
        __atomic_store(&list->head, &node, __ATOMIC_SEQ_CST);
        __atomic_store(&list->tail, &node, __ATOMIC_SEQ_CST);
    } else {
        /* general case */
        __atomic_store(&node->next, &old_head, __ATOMIC_SEQ_CST);
        __atomic_store(&old_head->prev, &node, __ATOMIC_SEQ_CST);
        __atomic_store(&list->head, &node, __ATOMIC_SEQ_CST);
    }

    unlock_for_write(list);
}

iterator_t list_find(rcu_list_t *list,
                     void *data,
                     finder_func_t finder,
                     read_handle_t *handle)
{
    iterator_t iter = {.entry = NULL}; /* initialize an invalid iterator */
    if (!list)
        return iter;

    list_node_t *current;
    __atomic_load(&list->head, &current, __ATOMIC_SEQ_CST);

    while (current) {
        if (finder(current->data, data)) {
            iter.entry = current;
            break;
        }

        __atomic_load(&current->next, &current, __ATOMIC_SEQ_CST);
    }
    return iter;
}

iterator_t list_begin(rcu_list_t *list, read_handle_t *handle)
{
    iterator_t iter = {.entry = NULL};
    if (!list)
        return iter;

    list_node_t *head;
    __atomic_load(&list->head, &head, __ATOMIC_SEQ_CST);

    iter.entry = head;
    return iter;
}

void *iterator_get(iterator_t *iter)
{
    return (iter && iter->entry) ? iter->entry->data : NULL;
}

read_handle_t list_register_reader(rcu_list_t *list)
{
    read_handle_t handle = {.list = list, .zombie = NULL};
    return handle;
}

write_handle_t list_register_writer(rcu_list_t *list)
{
    write_handle_t handle = {.list = list, .zombie = NULL};
    return handle;
}

void rcu_read_lock(read_handle_t *handle)
{
    zombie_node_t *z_node = make_zombie_node();

    z_node->owner = handle;
    handle->zombie = z_node;

    rcu_list_t *list = handle->list;

    zombie_node_t *old_head;
    __atomic_load(&list->zombie_head, &old_head, __ATOMIC_SEQ_CST);

    do {
        __atomic_store(&z_node->next, &old_head, __ATOMIC_SEQ_CST);

    } while (!__atomic_compare_exchange(&list->zombie_head, &old_head, &z_node,
                                        true, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST));
}

void rcu_read_unlock(read_handle_t *handle)
{
    zombie_node_t *z_node = handle->zombie;

    zombie_node_t *cached_next;
    __atomic_load(&z_node->next, &cached_next, __ATOMIC_SEQ_CST);

    bool last = true;

    /* walk through the zombie list to determine if this is the last active
     * reader in the list.
     */
    zombie_node_t *n = cached_next;
    while (n) {
        list_node_t *owner;
        __atomic_load(&n->owner, &owner, __ATOMIC_SEQ_CST);

        if (owner) {
            last = false; /* this is not the last active reader */
            break;
        }

        __atomic_load(&n->next, &n, __ATOMIC_SEQ_CST);
    }

    n = cached_next;

    if (last) {
        while (n) {
            list_node_t *dead_node = n->zombie;
            free(dead_node);

            zombie_node_t *old_node = n;
            __atomic_load(&n->next, &n, __ATOMIC_SEQ_CST);
            free(old_node);
        }

        __atomic_store(&z_node->next, &n, __ATOMIC_SEQ_CST);
    }

    void *null = NULL;
    __atomic_store(&z_node->owner, &null, __ATOMIC_SEQ_CST);
}

void rcu_write_lock(write_handle_t *handle)
{
    rcu_read_lock(handle);
}

void rcu_write_unlock(write_handle_t *handle)
{
    rcu_read_unlock(handle);
}

static list_node_t *make_node(void *data)
{
    list_node_t *node = malloc(sizeof(list_node_t));
    if (!node)
        return NULL;

    node->data = data;
    node->next = node->prev = NULL;
    node->deleted = false;

    return node;
}

static zombie_node_t *make_zombie_node(void)
{
    zombie_node_t *z_node = malloc(sizeof(zombie_node_t));
    if (!z_node)
        return NULL;

    z_node->zombie = NULL;
    z_node->owner = NULL;
    z_node->next = NULL;

    return z_node;
}

static void lock_for_write(rcu_list_t *list)
{
    pthread_mutex_lock(&list->write_lock);
}

static void unlock_for_write(rcu_list_t *list)
{
    pthread_mutex_unlock(&list->write_lock);
}

/* test program starts here */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct dummy {
    int a, b;
} dummy_t;

static dummy_t *make_dummy(int a, int b)
{
    dummy_t *dummy = malloc(sizeof(dummy_t));
    dummy->a = a, dummy->b = b;
    return dummy;
}

static bool finder(void *x, void *y)
{
    dummy_t *dx = x, *dy = y;
    return (dx->a == dy->a) && (dx->b == dy->b);
}

static void *reader_thread(void *arg)
{
    rcu_list_t *list = arg;
    read_handle_t reader = list_register_reader(list);

    rcu_read_lock(&reader);

    /* read from list here */
    iterator_t iter = list_find(list, &(dummy_t){1, 1}, finder, &reader);
    void *data = iterator_get(&iter);
    assert(data);

    iter = list_find(list, &(dummy_t){2, 2}, finder, &reader);
    data = iterator_get(&iter);
    assert(data);

    iterator_t first = list_begin(list, &reader);
    data = iterator_get(&first);
    assert(data);

    dummy_t *as_d2 = data;
    assert(as_d2->a == 2);
    assert(as_d2->b == 2);

    assert(iter.entry == first.entry);

    rcu_read_unlock(&reader);
    return NULL;
}

static void *writer_thread(void *arg)
{
    dummy_t *d1 = make_dummy(1, 1);
    dummy_t *d2 = make_dummy(2, 2);

    rcu_list_t *list = arg;
    write_handle_t writer = list_register_writer(list);

    rcu_write_lock(&writer);

    /* write to list here */
    list_push_front(list, d1, &writer);
    list_push_front(list, d2, &writer);

    rcu_write_unlock(&writer);
    return NULL;
}

#define N_READERS 10

int main(void)
{
    rcu_list_t *list = list_new();
    dummy_t *d0 = make_dummy(0, 0);
    list_push_front(list, d0, NULL);

    pthread_t t0, t_r[N_READERS];
    pthread_create(&t0, NULL, writer_thread, list);
    for (int i = 0; i < N_READERS; i++)
        pthread_create(&t_r[i], NULL, reader_thread, list);

    for (int i = 0; i < N_READERS; i++)
        pthread_join(t_r[i], NULL);
    pthread_join(t0, NULL);

    list_delete(list);

    return EXIT_SUCCESS;
}
