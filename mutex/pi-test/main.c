#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "atomic.h"
#include "mutex.h"

/* NOTE: This macro is memory-wasted because it will create
 * not must required static string for debugging purpose. */
#define TRY(f)                                                  \
    do {                                                        \
        int __r;                                                \
        if ((__r = (f != 0))) {                                 \
            fprintf(stderr, "Run function %s = %d\n", #f, __r); \
            return __r;                                         \
        }                                                       \
    } while (0)

struct ctx {
    mutex_t m0;
};

static void ctx_init(struct ctx *ctx)
{
    mutexattr_t mattr;
    mutexattr_setprotocol(&mattr, PRIO_INHERIT);
    mutex_init(&ctx->m0, &mattr);
}

static int pthread_create_with_prio(pthread_t *thread,
                                    pthread_attr_t *attr,
                                    void *(*start_routine)(void *),
                                    void *arg,
                                    int prio)
{
    struct sched_param param;
    param.sched_priority = prio;

    TRY(pthread_attr_setschedparam(attr, &param));
    TRY(pthread_create(thread, attr, start_routine, arg));

    return 0;
}

static atomic bool thread_stop = false;
static void *thread_low(void *arg)
{
    /* The low priority thread takes the lock shared with high priority
     * thread until it is finish. */
    struct ctx *ctx = (struct ctx *) arg;

    mutex_lock(&ctx->m0);
    sleep(1);
    mutex_unlock(&ctx->m0);

    return NULL;
}

static void *thread_mid(void *arg)
{
    /* The middle priority thread is very busy, so it may
     * block the high priority thread if priority inversion. */
    struct ctx *ctx = (struct ctx *) arg;

    while (!load(&thread_stop, relaxed))
        ;

    return NULL;
}

static void *thread_high(void *arg)
{
    struct ctx *ctx = (struct ctx *) arg;

    mutex_lock(&ctx->m0);
    /* If 'h' is printed, it means the high priority
     * thread obtain the lock because the low priority thread
     * releases it by priority inherit. */
    if (load(&thread_stop, relaxed) == false)
        printf("h");
    mutex_unlock(&ctx->m0);

    return NULL;
}

int main()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    TRY(pthread_attr_setschedpolicy(&attr, SCHED_FIFO));
    /* This enable the new created thread follow the attribute
     * which is provided in pthread_create */
    TRY(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED));

    struct ctx ctx;
    ctx_init(&ctx);

    pthread_t low_t, mid_t, high_t;
    /* Create the low priority thread, it comes first so the low priority thread
     * has more chance to get lock in prior. */
    TRY(pthread_create_with_prio(&low_t, &attr, thread_low, &ctx, 10));
    /* Create the middle priority thread */
    TRY(pthread_create_with_prio(&mid_t, &attr, thread_mid, &ctx, 20));
    /* Create the high priority thread */
    TRY(pthread_create_with_prio(&high_t, &attr, thread_high, &ctx, 30));

    sleep(2);
    store(&thread_stop, true, relaxed);

    TRY(pthread_join(low_t, NULL));
    TRY(pthread_join(mid_t, NULL));
    TRY(pthread_join(high_t, NULL));

    printf("\n");

    return 0;
}
