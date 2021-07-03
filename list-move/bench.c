#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#include "bench.h"

static void barrier_init(barrier_t *b, int n)
{
    pthread_cond_init(&b->complete, NULL);
    pthread_mutex_init(&b->mutex, NULL);
    b->count = n;
    b->crossing = 0;
}

static void barrier_cross(barrier_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->crossing++;
    if (b->crossing < b->count)
        pthread_cond_wait(&b->complete, &b->mutex);
    else {
        pthread_cond_broadcast(&b->complete);
        b->crossing = 0;
    }
    pthread_mutex_unlock(&b->mutex);
}

#define DEFAULT_DURATION 1000
#define DEFAULT_NTHREADS 64
#define DEFAULT_ISIZE 256
#define DEFAULT_VRANGE 512

static volatile bool should_stop = false;

static void *bench_thread(void *data)
{
    pthread_data_t *d = (pthread_data_t *) data;

    barrier_cross(d->barrier);
    while (!should_stop) {
        int from = rand_r(&d->seed) & 0x1;
        int key = rand_r(&d->seed) % d->range;
        list_move(key, d, from);
        d->n_move++;
    }

    return NULL;
}

int main(void)
{
    int duration = DEFAULT_DURATION;
    int n_threads = DEFAULT_NTHREADS;
    int init_size = DEFAULT_ISIZE;
    int value_range = DEFAULT_VRANGE;

    printf("List move benchmark\n");
    printf("Test time:     %d\n", duration);
    printf("Thread number: %d\n", n_threads);
    printf("Initial size:  %d\n", init_size);
    printf("Value range:   %d\n", value_range);

    struct timespec timeout = {.tv_sec = duration / 1000,
                               .tv_nsec = (duration % 1000) * 1000000};

    pthread_t *threads;
    if (!(threads = malloc(n_threads * sizeof(pthread_t)))) {
        printf("Failed to allocate pthread_t\n");
        goto out;
    }

    pthread_data_t **data;
    if (!(data = malloc(n_threads * sizeof(pthread_data_t *)))) {
        printf("Failed to allocate pthread_data_t\n");
        goto out;
    }
    for (int i = 0; i < n_threads; i++) {
        if ((data[i] = alloc_pthread_data()) == NULL) {
            printf("Failed to allocate pthread_data_t %d\n", i);
            goto out;
        }
    }

    srand(getpid() ^ (uintptr_t) main);

    void *list;
    if (!(list = list_global_init(init_size, value_range))) {
        printf("Failed to do list_global_init\n");
        goto out;
    }

    barrier_t barrier;
    pthread_attr_t attr;
    barrier_init(&barrier, n_threads + 1);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    for (int i = 0; i < n_threads; i++) {
        data[i]->id = i;
        data[i]->n_move = 0;
        data[i]->range = value_range;
        data[i]->seed = rand();
        data[i]->barrier = &barrier;
        data[i]->list = list;
        if (list_thread_init(data[i])) {
            printf("Failed to do list_thread_init\n");
            goto out;
        }
        if (pthread_create(&threads[i], &attr, bench_thread,
                           (void *) (data[i])) != 0) {
            printf("Failed to create thread %d\n", i);
            goto out;
        }
    }
    pthread_attr_destroy(&attr);

    barrier_cross(&barrier);

    struct timeval start, end;
    gettimeofday(&start, NULL);
    nanosleep(&timeout, NULL);
    should_stop = true;
    gettimeofday(&end, NULL);

    for (int i = 0; i < n_threads; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            printf("Failed to join child thread %d\n", i);
            goto out;
        }
    }

    duration = (end.tv_sec * 1000 + end.tv_usec / 1000) -
               (start.tv_sec * 1000 + start.tv_usec / 1000);
    unsigned long n_move = 0;
    for (int i = 0; i < n_threads; i++)
        n_move += (data[i]->n_move);

    printf("\tduration:     %d ms\n", duration);
    printf("\tops/second    %f/s\n", n_move * (1000.0) / duration);

    for (int i = 0; i < n_threads; i++)
        free_pthread_data(data[i]);
    list_global_exit(list);
    free(data);
    free(threads);

out:
    return 0;
}
