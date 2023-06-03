#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "broadcast.h"
#include "test.h"

#define MAX_THREADS 128

typedef struct thread_state thread_state_t;
struct thread_state {
    pthread_t thread;
    broadcast_t *bcast;

    bool pub;
    uint32_t pub_id;
    size_t pub_msgs, sub_msgs;

    size_t n_msgs, n_drops;
    int64_t dt;
};

static void thread_func_pub(thread_state_t *t)
{
    broadcast_t *b = t->bcast;
    uint64_t msg = (uint64_t) t->pub_id << 32;
    int64_t start_time = wallclock();

    for (size_t i = 0; i < t->pub_msgs; i++) {
        msg++;
        bool success = broadcast_pub(b, &msg, sizeof(msg));
        assert(success);
        t->n_msgs++;
    }

    t->dt = wallclock() - start_time;
}

static void thread_func_sub(thread_state_t *t)
{
    broadcast_t *b = t->bcast;
    int64_t start_time = wallclock();

    broadcast_sub_t sub[1];
    broadcast_sub_begin(sub, b);

    uint64_t msg;
    size_t msg_size;
    size_t drops;
    for (size_t i = 0; i < (size_t) 1e9; i++) {
        if (!broadcast_sub_next(sub, &msg, &msg_size, &drops))
            continue;
        assert(msg_size == sizeof(msg));

        uint32_t pub_id = msg >> 32;
        assert(pub_id < MAX_THREADS);

        t->n_msgs++;
        t->n_drops += drops;
        if (t->n_msgs + t->n_drops == t->sub_msgs)
            break;
    }

    t->dt = wallclock() - start_time;
}

static void *thread_func(void *usr)
{
    thread_state_t *t = (thread_state_t *) usr;
    if (t->pub)
        thread_func_pub(t);
    else
        thread_func_sub(t);
    return NULL;
}

static void run_test(const char *test_name,
                     size_t num_pub,
                     size_t num_sub,
                     size_t num_elts)
{
    broadcast_t *b = broadcast_new(num_elts, sizeof(uint64_t));
    if (!b)
        FAIL("Failed to create new bcast");

    size_t pub_msgs = (size_t) 1e5;
    size_t sub_msgs = num_pub * pub_msgs;

    assert(num_sub < MAX_THREADS);
    thread_state_t sub_threads[MAX_THREADS] = {{}};
    for (size_t i = 0; i < num_sub; i++) {
        thread_state_t *t = &sub_threads[i];
        t->bcast = b;
        t->pub = false;
        t->sub_msgs = sub_msgs;
        pthread_create(&t->thread, NULL, thread_func, t);
    }

    assert(num_pub < MAX_THREADS);
    thread_state_t pub_threads[MAX_THREADS] = {{}};
    for (size_t i = 0; i < num_pub; i++) {
        thread_state_t *t = &pub_threads[i];
        t->bcast = b;
        t->pub = true;
        t->pub_id = (uint32_t) i;
        t->pub_msgs = pub_msgs;
        pthread_create(&t->thread, NULL, thread_func, t);
    }

    for (size_t i = 0; i < num_sub; i++)
        pthread_join(sub_threads[i].thread, NULL);
    for (size_t i = 0; i < num_pub; i++)
        pthread_join(pub_threads[i].thread, NULL);

    broadcast_delete(b);

    /* Report stats */
    printf("Test: %s\n", test_name);
    for (size_t i = 0; i < num_sub; i++) {
        thread_state_t *t = &sub_threads[i];
        printf("  Sub Thread %zu | n_msgs: %7zu n_drops: %7zu | %.f ns/msg\n",
               i, t->n_msgs, t->n_drops, (double) t->dt / t->n_msgs);
    }
    for (size_t i = 0; i < num_pub; i++) {
        thread_state_t *t = &pub_threads[i];
        printf("  Pub Thread %zu | n_msgs: %7zu n_drops: %7zu | %.f ns/msg\n",
               i, t->n_msgs, t->n_drops, (double) t->dt / t->n_msgs);
    }
}

int main()
{
    /* Stress publishing, rolling around with lots of contention */
    run_test("1pub0sub", 1, 0, 128);
    run_test("2pub0sub", 2, 0, 128);
    run_test("4pub0sub", 4, 0, 128);
    run_test("4pub0sub", 8, 0, 128);
    printf("\n");

    /* Stress publishing with 1 sub */
    run_test("1pub1sub", 1, 1, 2048);
    run_test("2pub1sub", 2, 1, 2048);
    run_test("4pub1sub", 4, 1, 2048);
    run_test("8pub1sub", 8, 1, 2048);
    printf("\n");

    /* Stress publishing with 2 subs */
    run_test("1pub2sub", 1, 2, 2048);
    run_test("2pub2sub", 2, 2, 2048);
    run_test("4pub2sub", 4, 2, 2048);
    run_test("8pub2sub", 8, 2, 2048);
    printf("\n");

    /* Stress subscribing */
    run_test("1pub1sub", 1, 1, 2048);
    run_test("1pub2sub", 1, 2, 2048);
    run_test("1pub4sub", 1, 4, 2048);
    run_test("1pub8sub", 1, 8, 2048);
    printf("\n");

    return 0;
}
