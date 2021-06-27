#include <assert.h>

#include "bench.h"

typedef struct node {
    int val;
    struct node *next;
} node_t;

typedef struct spinlock_list {
    pthread_spinlock_t spinlock;
    node_t *head[2];
} spinlock_list_t;

pthread_data_t *alloc_pthread_data(void)
{
    size_t size = sizeof(pthread_data_t);
    size = CACHE_ALIGN(size);

    pthread_data_t *d = malloc(size);
    if (d)
        d->ds_data = NULL;

    return d;
}

void free_pthread_data(pthread_data_t *d)
{
    free(d);
}

void *list_global_init(int size, int value_range)
{
    spinlock_list_t *list = malloc(sizeof(spinlock_list_t));
    if (!list)
        return NULL;

    pthread_spin_init(&list->spinlock, PTHREAD_PROCESS_PRIVATE);
    list->head[0] = malloc(sizeof(node_t));
    list->head[1] = malloc(sizeof(node_t));
    if (!list->head[0] || !list->head[1])
        return NULL;
    node_t *node[2] = {list->head[0], list->head[1]};
    node[0]->val = INT_MIN;
    node[1]->val = INT_MIN;

    for (int i = 0; i < value_range; i += value_range / size) {
        node[0]->next = malloc(sizeof(node_t));
        node[1]->next = malloc(sizeof(node_t));
        if (!node[0]->next || !node[1]->next)
            return NULL;
        node[0] = node[0]->next;
        node[1] = node[1]->next;
        node[0]->val = i;
        node[1]->val = i + 1;
    }
    node[0]->next = malloc(sizeof(node_t));
    node[1]->next = malloc(sizeof(node_t));
    if (!node[0]->next || !node[1]->next)
        return NULL;
    node[0]->val = node[1]->val = INT_MAX;

    return list;
}

int list_thread_init(pthread_data_t *data)
{
    return 0;
}

void list_global_exit(void *list)
{
    spinlock_list_t *l = (spinlock_list_t *) list;
    pthread_spin_destroy(&l->spinlock);
}

int list_move(int key, pthread_data_t *data, int from)
{
    spinlock_list_t *list = (spinlock_list_t *) data->list;
    node_t *prev_src, *cur, *prev_dst, *next_dst;
    int val;

    pthread_spin_lock(&list->spinlock);
    for (prev_src = list->head[from], cur = prev_src->next; cur;
         prev_src = cur, cur = cur->next)
        if ((val = cur->val) >= key)
            break;

    int ret = (val == key);
    if (!ret)
        goto out;
    for (prev_dst = list->head[1 - from], next_dst = prev_dst->next; next_dst;
         prev_dst = next_dst, next_dst = next_dst->next)
        if ((val = next_dst->val) >= key)
            break;
    ret = (val != key);
    if (!ret)
        goto out;

    prev_src->next = cur->next;
    prev_dst->next = cur;
    cur->next = next_dst;
out:
    pthread_spin_unlock(&list->spinlock);

    return ret;
}
