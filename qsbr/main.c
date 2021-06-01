#include <assert.h>
#include <stdbool.h>

/* Branch prediction hints */
#define unlikely(x) __builtin_expect((x) != 0, 0)

#ifndef atomic_fetch_add
#define atomic_fetch_add(x, a) __sync_fetch_and_add(x, a)
#endif

#ifndef atomic_thread_fence
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_seq_cst __ATOMIC_SEQ_CST
#define atomic_thread_fence(m) __atomic_thread_fence(m)
#endif

#ifndef atomic_store_explicit
#define atomic_store_explicit __atomic_store_n
#endif

#ifndef atomic_load_explicit
#define atomic_load_explicit __atomic_load_n
#endif

/* Exponential back-off for the spinning paths */
#define SPINLOCK_BACKOFF_MIN 4
#define SPINLOCK_BACKOFF_MAX 128
#if defined(__x86_64__) || defined(__i386__)
#define SPINLOCK_BACKOFF_HOOK __asm volatile("pause" ::: "memory")
#else
#define SPINLOCK_BACKOFF_HOOK \
    do {                      \
    } while (0)
#endif
#define SPINLOCK_BACKOFF(count)                    \
    do {                                           \
        for (int __i = (count); __i != 0; __i--) { \
            SPINLOCK_BACKOFF_HOOK;                 \
        }                                          \
        if ((count) < SPINLOCK_BACKOFF_MAX)        \
            (count) += (count);                    \
    } while (0)

#define CACHE_LINE_SIZE 64

/* Quiescent state based reclamation (QSBR).
 *
 * Each registered thread has to periodically indicate that it is in a
 * quiescent i.e. the state when it does not hold any memory references to the
 * objects which may be garbage collected. A typical use of the qsbr_checkpoint
 * function would be e.g. after processing a single request when any shared
 * state is no longer referenced. The higher the period, the higher the
 * reclamation granularity.
 *
 * Writers i.e. threads which are trying to garbage collect the object should
 * ensure that the objects are no longer globally visible and then issue a
 * barrier using qsbr_barrier function. This function returns a generation
 * number. It is safe to reclaim the said objects when qsbr_sync returns true
 * on a given number.
 *
 * Note that this interface is asynchronous.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

typedef uint64_t qsbr_epoch_t;

typedef struct qsbr_tls {
    /* The thread (local) epoch, observed at qsbr_checkpoint. Also, a pointer
     * to the TLS structure of a next thread.
     */
    qsbr_epoch_t local_epoch;
    LIST_ENTRY(qsbr_tls) entry;
} qsbr_tls_t;

typedef struct qsbr {
    /* The global epoch, TLS key with a list of the registered threads. */
    qsbr_epoch_t global_epoch;
    pthread_key_t tls_key;
    pthread_mutex_t lock;
    LIST_HEAD(priv, qsbr_tls) list;
} qsbr_t;

qsbr_t *qsbr_create(void)
{
    qsbr_t *qs;
    int ret = posix_memalign((void **) &qs, CACHE_LINE_SIZE, sizeof(qsbr_t));
    if (ret != 0) {
        errno = ret;
        return NULL;
    }
    memset(qs, 0, sizeof(qsbr_t));

    if (pthread_key_create(&qs->tls_key, free) != 0) {
        free(qs);
        return NULL;
    }
    pthread_mutex_init(&qs->lock, NULL);
    qs->global_epoch = 1;
    return qs;
}

void qsbr_destroy(qsbr_t *qs)
{
    pthread_key_delete(qs->tls_key);
    pthread_mutex_destroy(&qs->lock);
    free(qs);
}

/* qsbr_register: register the current thread for QSBR. */
int qsbr_register(qsbr_t *qs)
{
    qsbr_tls_t *t = pthread_getspecific(qs->tls_key);
    if (unlikely(!t)) {
        int ret =
            posix_memalign((void **) &t, CACHE_LINE_SIZE, sizeof(qsbr_tls_t));
        if (ret != 0) {
            errno = ret;
            return -1;
        }
        pthread_setspecific(qs->tls_key, t);
    }
    memset(t, 0, sizeof(qsbr_tls_t));

    pthread_mutex_lock(&qs->lock);
    LIST_INSERT_HEAD(&qs->list, t, entry);
    pthread_mutex_unlock(&qs->lock);
    return 0;
}

void qsbr_unregister(qsbr_t *qsbr)
{
    qsbr_tls_t *t = pthread_getspecific(qsbr->tls_key);
    if (!t)
        return;

    pthread_setspecific(qsbr->tls_key, NULL);

    pthread_mutex_lock(&qsbr->lock);
    LIST_REMOVE(t, entry);
    pthread_mutex_unlock(&qsbr->lock);
    free(t);
}

/* qsbr_checkpoint: indicate a quiescent state of the current thread. */
void qsbr_checkpoint(qsbr_t *qs)
{
    qsbr_tls_t *t = pthread_getspecific(qs->tls_key);
    assert(t);

    /* Observe the current epoch and issue a load barrier.
     *
     * Additionally, issue a store barrier before observation, so the callers
     * could assume qsbr_checkpoint() being a full barrier.
     */
    atomic_thread_fence(memory_order_seq_cst);
    t->local_epoch = qs->global_epoch;
}

qsbr_epoch_t qsbr_barrier(qsbr_t *qs)
{
    /* Note: atomic operation will issue a store barrier. */
    return atomic_fetch_add(&qs->global_epoch, 1) + 1;
}

bool qsbr_sync(qsbr_t *qs, qsbr_epoch_t target)
{
    /* First, our thread should observe the epoch itself. */
    qsbr_checkpoint(qs);

    /* Have all threads observed the target epoch? */
    qsbr_tls_t *t;
    LIST_FOREACH (t, &qs->list, entry) {
        if (t->local_epoch < target) /* not ready to reclaim */
            return false;
    }

    /* Detected the grace period */
    return true;
}

/* Test program starts here */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static unsigned nsec = 10; /* seconds */

static pthread_barrier_t barrier;
static unsigned n_workers;
static volatile bool stop;

typedef struct {
    unsigned int *ptr;
    bool visible;
    char _pad[CACHE_LINE_SIZE - 8 - 4 - 4 - 8];
} data_struct_t;

#define N_DS 4

#define MAGIC 0xDEADBEEF
static unsigned magic_val = MAGIC;

static qsbr_t *qsbr;

static data_struct_t ds[N_DS] __attribute__((__aligned__(CACHE_LINE_SIZE)));
static uint64_t destructions;

static void access_obj(data_struct_t *obj)
{
    if (atomic_load_explicit(&obj->visible, memory_order_relaxed)) {
        atomic_thread_fence(memory_order_acquire);
        if (*obj->ptr != MAGIC)
            abort();
    }
}

static void mock_insert_obj(data_struct_t *obj)
{
    obj->ptr = &magic_val;
    assert(!obj->visible);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&obj->visible, true, memory_order_relaxed);
}

static void mock_remove_obj(data_struct_t *obj)
{
    assert(obj->visible);
    obj->visible = false;
}

static void mock_destroy_obj(data_struct_t *obj)
{
    obj->ptr = NULL;
    destructions++;
}

/* QSBR stress test */

static void qsbr_writer(unsigned target)
{
    data_struct_t *obj = &ds[target];

    if (obj->visible) {
        /* The data structure is visible. First, ensure it is no longer
         * visible (think of "remove" semantics).
         */
        unsigned count = SPINLOCK_BACKOFF_MIN;
        qsbr_epoch_t target_epoch;

        mock_remove_obj(obj);

        /* QSBR synchronization barrier. */
        target_epoch = qsbr_barrier(qsbr);
        while (!qsbr_sync(qsbr, target_epoch)) {
            SPINLOCK_BACKOFF(count);
            /* Other threads might have exited and the checkpoint would never
             * be passed.
             */
            if (stop)
                return;
        }

        /* It is safe to "destroy" the object now. */
        mock_destroy_obj(obj);
    } else {
        /* Data structure is not globally visible. Set the value and make it
         * visible (think of the "insert" semantics).
         */
        mock_insert_obj(obj);
    }
}

static void *qsbr_stress(void *arg)
{
    const unsigned id = (uintptr_t) arg;
    unsigned n = 0;

    qsbr_register(qsbr);

    /* There are NCPU threads concurrently reading data and a single writer
     * thread (ID 0) modifying data. The writer will modify the pointer used
     * by the readers to NULL as soon as it considers the object ready for
     * reclaim.
     */
    pthread_barrier_wait(&barrier);
    while (!stop) {
        n = (n + 1) & (N_DS - 1);
        if (id == 0) {
            qsbr_writer(n);
            continue;
        }

        /* Reader: iterate through the data structures and if the object is
         * visible (think of "lookup" semantics), read its value through a
         * pointer. The writer will set the pointer to NULL when it thinks
         * the object is ready to be reclaimed.
         *
         * Incorrect reclamation mechanism would lead to the crash in the
         * following pointer dereference.
         */
        access_obj(&ds[n]);
        qsbr_checkpoint(qsbr);
    }
    pthread_barrier_wait(&barrier);
    qsbr_unregister(qsbr);
    pthread_exit(NULL);
    return NULL;
}

static void leave(int sig)
{
    (void) sig;
    stop = true;
}

typedef void *(*func_t)(void *);

static void run_test(func_t func)
{
    struct sigaction sigalarm;

    n_workers = sysconf(_SC_NPROCESSORS_CONF);
    pthread_t *thr = calloc(n_workers, sizeof(pthread_t));
    pthread_barrier_init(&barrier, NULL, n_workers);
    stop = false;

    memset(&sigalarm, 0, sizeof(struct sigaction));
    sigalarm.sa_handler = leave;
    int ret = sigaction(SIGALRM, &sigalarm, NULL);
    assert(ret == 0);

    memset(&ds, 0, sizeof(ds));
    qsbr = qsbr_create();
    destructions = 0;

    alarm(nsec); /* Spin the test */

    for (unsigned i = 0; i < n_workers; i++) {
        if ((errno = pthread_create(&thr[i], NULL, func,
                                    (void *) (uintptr_t) i)) != 0) {
            exit(EXIT_FAILURE);
        }
    }
    for (unsigned i = 0; i < n_workers; i++)
        pthread_join(thr[i], NULL);
    pthread_barrier_destroy(&barrier);
    printf("# %" PRIu64 "\n", destructions);

    qsbr_destroy(qsbr);
}

int main(int argc, char **argv)
{
    printf("stress test...\n");
    run_test(qsbr_stress);
    printf("OK\n");
    return 0;
}
