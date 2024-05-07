/* Copyright (c) 2013 David Wragg

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* These codes are referenced to:
 * - https://github.com/jserv/skinny-mutex/blob/master/perf.c */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "mutex.h"

#define max(a, b)           \
    ({                      \
        typeof(a) _a = (a); \
        typeof(b) _b = (b); \
        _a > _b ? _a : _b;  \
    })

#define min(a, b)           \
    ({                      \
        typeof(a) _a = (a); \
        typeof(b) _b = (b); \
        _a < _b ? _a : _b;  \
    })

#define MEASURE_TIME 100

#define THREAD_MAX 4
#define LOCK_MAX (THREAD_MAX + 1)

#define THREAD_ITERS \
    50  // The times for a thread that tries to acquire the next lock
#define TOTAL_ITERS (THREAD_MAX * THREAD_ITERS)

static long long ns_time()
{
    struct timespec tt;
    clock_gettime(CLOCK_MONOTONIC, &tt);
    return tt.tv_sec * 1e9 + tt.tv_nsec;
}

struct common {
    /* As https://github.com/jserv/skinny-mutex/blob/master/perf.c#L116 explain:
     *
     * Here the numbers of mutex is the number of threads add one, and those are
     * arranged in a ring. Each thread will hold a lock first, and also tries to
     * acquire the next lock in the ring. When it acquires the next lock, it
     * drops the previous lock. Then it tries to acquire the next next lock, and
     * so on.
     *
     * The effect is that at every moment, only one thread is able to acquire
     * two locks and so make progress; in doing so, it releases a lock allowing
     * another thread to make progress and then promptly gets blocked.*/
    mutex_t mutexes[LOCK_MAX];

    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_cond;
    int ready_count;
};

/* Every thread will have its own 'thread_ctx' structure.
 * It contains a 'common' structure which references to
 * some share information between any other threads, and
 * the remaining structure members are independent for each
 * thread. */
struct thread_ctx {
    struct common *common;
    pthread_mutex_t start_mutex;
    int id;

    long long start;
    long long stop;
};

static void *thread_func(void *arg)
{
    struct thread_ctx *ctx = (struct thread_ctx *) arg;
    struct common *common = ctx->common;
    int i = ctx->id;

    /* Lock our first mutex */
    mutex_lock(&common->mutexes[i]);

    /* Every thread will signal the main thread. So after main thread get
     * enough numbers of signal, it knows that everyones are on the starting
     * line. */
    pthread_mutex_lock(&common->ready_mutex);
    if (++common->ready_count == THREAD_MAX)
        pthread_cond_signal(&common->ready_cond);
    pthread_mutex_unlock(&common->ready_mutex);

    /* Line up to start */
    pthread_mutex_lock(&ctx->start_mutex);
    pthread_mutex_unlock(&ctx->start_mutex);

    ctx->start = ns_time();

    /* Core routine here */
    for (int loop = 1; loop < THREAD_ITERS; loop++) {
        int next = (i + 1) % LOCK_MAX;
        mutex_lock(&common->mutexes[next]);
        mutex_unlock(&common->mutexes[i]);
        i = next;
    }

    ctx->stop = ns_time();

    mutex_unlock(&common->mutexes[i]);
    return NULL;
}

static void init_common(struct common *common)
{
    for (int i = 0; i < LOCK_MAX; i++)
        mutex_init(&common->mutexes[i], NULL);

    pthread_mutex_init(&common->ready_mutex, NULL);
    pthread_cond_init(&common->ready_cond, NULL);
    common->ready_count = 0;
}

static void init_ctx(struct thread_ctx *ctx, struct common *common, int id)
{
    ctx->common = common;
    ctx->id = id;

    pthread_mutex_init(&ctx->start_mutex, NULL);
}

static void free_common(struct common *common)
{
    for (int i = 0; i < LOCK_MAX; i++)
        mutex_destroy(&common->mutexes[i]);

    pthread_mutex_destroy(&common->ready_mutex);
    pthread_cond_destroy(&common->ready_cond);
}

static void free_ctx(struct thread_ctx *ctx)
{
    pthread_mutex_destroy(&ctx->start_mutex);
}

static long long contention_test()
{
    struct common common;
    struct thread_ctx ctx[THREAD_MAX];
    pthread_t threads[THREAD_MAX];

    init_common(&common);

    for (int i = 0; i < THREAD_MAX; i++) {
        init_ctx(&ctx[i], &common, i);

        /* Lock before creating the thread, so we can run their main routine
         * after every thread are created. */
        pthread_mutex_lock(&ctx[i].start_mutex);
        pthread_create(&threads[i], NULL, thread_func, &ctx[i]);
    }

    /* Wait until every thread signal the main thread */
    pthread_mutex_lock(&common.ready_mutex);
    while (common.ready_count < THREAD_MAX)
        pthread_cond_wait(&common.ready_cond, &common.ready_mutex);
    pthread_mutex_unlock(&common.ready_mutex);

    /* Great! Let's release the lock so every thread can start the
     * core routine. */
    for (int i = 0; i < THREAD_MAX; i++)
        pthread_mutex_unlock(&ctx[i].start_mutex);

    for (int i = 0; i < THREAD_MAX; i++) {
        pthread_join(threads[i], NULL);
        free_ctx(&ctx[i]);
    }

    free_common(&common);

    /* Let's check the time range to finish TOTAL_ITERS times of
     * lock contention. */
    long long start = ctx[0].start;
    long long stop = ctx[0].stop;
    for (int i = 1; i < THREAD_MAX; i++) {
        start = min(ctx[i].start, start);
        stop = max(ctx[i].stop, stop);
    }

    return stop - start;
}

static int cmp(const void *ap, const void *bp)
{
    long long a = *(long long *) ap;
    long long b = *(long long *) bp;
    return a - b;
}

int main()
{
    long long times[MEASURE_TIME];
    long long total_time = 0;
    for (int i = 0; i < MEASURE_TIME; i++) {
        times[i] = contention_test();
        total_time += times[i];
    }

    qsort(times, MEASURE_TIME, sizeof(long long), cmp);
    for (int i = 0; i < MEASURE_TIME; i++) {
        printf("%lld, ", times[i]);
    }
    printf("\n");

    // printf("Average %lld\n", total_time/MEASURE_TIME);

    return 0;
}
