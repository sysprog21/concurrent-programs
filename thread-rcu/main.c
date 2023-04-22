#include <pthread.h>
#include <stdatomic.h>

struct barrier_struct {
    atomic_int flag;
    int count;
    pthread_mutex_t lock;
};

static __thread int local_sense = 0;

#define BARRIER_INIT                                             \
    {                                                            \
        .flag = 0, .count = 0, .lock = PTHREAD_MUTEX_INITIALIZER \
    }

#define DEFINE_BARRIER(name) struct barrier_struct name = BARRIER_INIT

static inline void thread_barrier(struct barrier_struct *b, size_t n)
{
    local_sense = !local_sense;

    pthread_mutex_lock(&b->lock);
    b->count++;
    if (b->count == n) {
        b->count = 0;
        pthread_mutex_unlock(&b->lock);
        atomic_store_explicit(&b->flag, local_sense, memory_order_release);
    } else {
        pthread_mutex_unlock(&b->lock);
        while (atomic_load_explicit(&b->flag, memory_order_acquire) !=
               local_sense)
            ;
    }
}

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rcu.h"

#define GP_IDX_MAX N_UPDATE_RUN + 1

static DEFINE_BARRIER(test_barrier);

struct test {
    unsigned int count;
};
static struct test __rcu *dut;
static atomic_uint gp_idx;
static atomic_uint prev_count;
static atomic_uint grace_periods[GP_IDX_MAX];

static void *reader_func(void *argv)
{
    struct test *tmp;
    unsigned int local_gp_idx;
    unsigned int old_prev_count;
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 30000L,
    };

    if (rcu_init())
        abort();

    thread_barrier(&test_barrier, N_READERS + 1);
    nanosleep(&ts, NULL);

    rcu_read_lock();

    tmp = rcu_dereference(dut);

    old_prev_count = atomic_load_explicit(&prev_count, memory_order_acquire);
    if (old_prev_count < tmp->count) {
        atomic_compare_exchange_strong(&prev_count, &old_prev_count,
                                       tmp->count);
    } else if (tmp->count < old_prev_count) {
        fprintf(stderr,
                "old count (%u) should not be larger than new one (%u).\n",
                old_prev_count, tmp->count);
        abort();
    }

    local_gp_idx = atomic_load_explicit(&gp_idx, memory_order_acquire);
    if (local_gp_idx > N_UPDATE_RUN) {
        fprintf(stderr, "grace period index (%u) is over bound (%u).\n",
                local_gp_idx, N_UPDATE_RUN);
        abort();
    }
    atomic_fetch_add_explicit(&grace_periods[local_gp_idx], 1,
                              memory_order_relaxed);

    rcu_read_unlock();

    pthread_exit(NULL);
}

static void *updater_func(void *argv)
{
    struct test *oldp;
    struct test *newval;
    unsigned int i = 0;

    thread_barrier(&test_barrier, N_READERS + 1);
    atomic_thread_fence(memory_order_seq_cst);

    while (i++ < N_UPDATE_RUN) {
        newval = malloc(sizeof(struct test));
        newval->count = i;
        oldp = rcu_assign_pointer(dut, newval);
        synchronize_rcu();
        atomic_fetch_add_explicit(&gp_idx, 1, memory_order_release);
        free(oldp);
    }

    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    pthread_t reader[N_READERS];
    pthread_t updater;
    unsigned int i, total = 0;

    dut = (struct test __rcu *) malloc(sizeof(struct test));
    rcu_uncheck(dut)->count = 0;

    for (i = 0; i < N_READERS; i++)
        pthread_create(&reader[i], NULL, reader_func, NULL);
    pthread_create(&updater, NULL, updater_func, NULL);

    for (i = 0; i < N_READERS; i++)
        pthread_join(reader[i], NULL);
    pthread_join(updater, NULL);

    free(rcu_uncheck(dut));
    rcu_clean();

    atomic_thread_fence(memory_order_seq_cst);

    printf("%u reader(s), %u update run(s), %u grace period(s)\n", N_READERS,
           N_UPDATE_RUN, gp_idx + 1);
    for (i = 0; i < gp_idx + 1; i++) {
        printf("[grace period #%u] %4u reader(s)\n", i, grace_periods[i]);
        total += grace_periods[i];
    }

    if (total != N_READERS)
        fprintf(stderr,
                "The sum of records in the array of grace period(s) (%u)\n"
                "is not the same with number of reader(s) (%u)\n",
                total, N_READERS);

    return 0;
}
