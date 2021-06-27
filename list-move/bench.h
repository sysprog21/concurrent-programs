#ifndef BENCH_MOVE_LIST_H
#define BENCH_MOVE_LIST_H

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_cond_t complete;
    pthread_mutex_t mutex;
    int count, crossing;
} barrier_t;

#define CACHE_LINE (64)
#define CACHE_ALIGN(size) ((((size - 1) / CACHE_LINE) + 1) * CACHE_LINE)

#define PTHREAD_PADDING (16)

typedef struct {
    long pthread_padding[PTHREAD_PADDING];
    long id;
    unsigned long n_move;
    int range;
    unsigned int seed;
    barrier_t *barrier;
    void *list;
    void *ds_data; /* data structure specific data */
} pthread_data_t;

pthread_data_t *alloc_pthread_data(void);

void free_pthread_data(pthread_data_t *d);
void *list_global_init(int init_size, int value_range);
int list_thread_init(pthread_data_t *data);
void list_global_exit(void *list);
int list_move(int key, pthread_data_t *data, int from);

#endif
