#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cond.h"
#include "futex.h"
#include "mutex.h"

struct clock {
    mutex_t mutex;
    cond_t cond;
    int ticks;
};

static void clock_init(struct clock *clock)
{
    mutex_init(&clock->mutex, NULL);
    cond_init(&clock->cond);
    clock->ticks = 0;
}

static bool clock_wait(struct clock *clock, int ticks)
{
    mutex_lock(&clock->mutex);
    while (clock->ticks >= 0 && clock->ticks < ticks)
        cond_wait(&clock->cond, &clock->mutex);
    bool ret = clock->ticks >= ticks;
    mutex_unlock(&clock->mutex);
    return ret;
}

static void clock_tick(struct clock *clock)
{
    mutex_lock(&clock->mutex);
    if (clock->ticks >= 0)
        ++clock->ticks;
    mutex_unlock(&clock->mutex);
    cond_broadcast(&clock->cond, &clock->mutex);
}

static void clock_stop(struct clock *clock)
{
    mutex_lock(&clock->mutex);
    clock->ticks = -1;
    mutex_unlock(&clock->mutex);
    cond_broadcast(&clock->cond, &clock->mutex);
}

/* A node in a computation graph */
struct node {
    struct clock *clock;
    struct node *parent;
    mutex_t mutex;
    cond_t cond;
    bool ready;
};

static void node_init(struct clock *clock,
                      struct node *parent,
                      struct node *node)
{
    node->clock = clock;
    node->parent = parent;
    mutex_init(&node->mutex, NULL);
    cond_init(&node->cond);
    node->ready = false;
}

static void node_wait(struct node *node)
{
    mutex_lock(&node->mutex);
    while (!node->ready)
        cond_wait(&node->cond, &node->mutex);
    node->ready = false;
    mutex_unlock(&node->mutex);
}

static void node_signal(struct node *node)
{
    mutex_lock(&node->mutex);
    node->ready = true;
    mutex_unlock(&node->mutex);
    cond_signal(&node->cond, &node->mutex);
}

static void *thread_func(void *ptr)
{
    struct node *self = ptr;
    bool bit = false;

    for (int i = 1; clock_wait(self->clock, i); ++i) {
        if (self->parent)
            node_wait(self->parent);

        if (bit) {
            node_signal(self);
        } else {
            clock_tick(self->clock);
        }
        bit = !bit;
    }

    node_signal(self);
    return NULL;
}

int main(void)
{
    struct clock clock;
    clock_init(&clock);

#define N_NODES 16
    struct node nodes[N_NODES];
    node_init(&clock, NULL, &nodes[0]);
    for (int i = 1; i < N_NODES; ++i)
        node_init(&clock, &nodes[i - 1], &nodes[i]);

    pthread_t threads[N_NODES];
    for (int i = 0; i < N_NODES; ++i) {
        if (pthread_create(&threads[i], NULL, thread_func, &nodes[i]) != 0)
            return EXIT_FAILURE;
    }

    clock_tick(&clock);
    clock_wait(&clock, 1 << N_NODES);
    clock_stop(&clock);

    for (int i = 0; i < N_NODES; ++i) {
        if (pthread_join(threads[i], NULL) != 0)
            return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
