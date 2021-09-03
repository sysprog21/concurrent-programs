#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "free_later.h"
#include "hashmap.h"

/* global hash map */
static hashmap_t *map = NULL;

/* how many threads should run in parallel */
#define N_THREADS 32

/* how many times the work loop should repeat */
#define N_LOOPS 100

/* state for the threads */
static pthread_t threads[N_THREADS];

/* state for the threads that test deletes */
static pthread_t threads_del[N_THREADS * 2];

static uint32_t MAX_VAL_PLUS_ONE = N_THREADS * N_LOOPS + 1;

extern volatile uint32_t hashmap_del_fail, hashmap_del_fail_new_head;
extern volatile uint32_t hashmap_put_retries, hashmap_put_replace_fail;
extern volatile uint32_t hashmap_put_head_fail;

static uint8_t cmp_uint32(const void *x, const void *y)
{
    uint32_t xi = *(uint32_t *) x, yi = *(uint32_t *) y;
    if (xi > yi)
        return -1;
    if (xi < yi)
        return 1;
    return 0;
}

static uint64_t hash_uint32(const void *key)
{
    return *(uint32_t *) key;
}

/* Simulates work that is quick and uses the hashtable once per loop */
static void *add_vals(void *args)
{
    int *offset = args;
    for (int j = 0; j < N_LOOPS; j++) {
        int *val = malloc(sizeof(int));
        *val = (*offset * N_LOOPS) + j;
        hashmap_put(map, val, val);
    }
    return NULL;
}

bool mt_add_vals(void)
{
    for (int i = 0; i < N_THREADS; i++) {
        int *offset = malloc(sizeof(int));
        *offset = i;
        if (pthread_create(&threads[i], NULL, add_vals, offset) != 0) {
            printf("Failed to create thread %d\n", i);
            exit(1);
        }
    }
    // wait for work to finish
    for (int i = 0; i < N_THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            printf("Failed to join thread %d\n", i);
            exit(1);
        }
    }
    return true;
}

/* add a value over and over to test the del functionality */
void *add_val(void *args)
{
    for (int j = 0; j < N_LOOPS; j++)
        hashmap_put(map, &MAX_VAL_PLUS_ONE, &MAX_VAL_PLUS_ONE);
    return NULL;
}

static void *del_val(void *args)
{
    for (int j = 0; j < N_LOOPS; j++)
        hashmap_del(map, &MAX_VAL_PLUS_ONE);
    return NULL;
}

bool mt_del_vals(void)
{
    for (int i = 0; i < N_THREADS; i++) {
        if (pthread_create(&threads_del[i], NULL, add_val, NULL) != 0) {
            printf("Failed to create thread %d\n", i);
            exit(1);
        }
        if (pthread_create(&threads_del[N_THREADS + i], NULL, del_val, NULL)) {
            printf("Failed to create thread %d\n", i);
            exit(1);
        }
    }
    // also add normal numbers to ensure they aren't clobbered
    mt_add_vals();
    // wait for work to finish
    for (int i = 0; i < N_THREADS * 2; i++) {
        if (pthread_join(threads_del[i], NULL) != 0) {
            printf("Failed to join thread %d\n", i);
            exit(1);
        }
    }
    return true;
}

bool test_add()
{
    map = hashmap_new(10, cmp_uint32, hash_uint32);

    int loops = 0;
    while (hashmap_put_retries == 0) {
        loops += 1;
        if (!mt_add_vals()) {
            printf("Error. Failed to add values!\n");
            return false;
        }

        /* check all the list entries */
        uint32_t TOTAL = N_THREADS * N_LOOPS;
        uint32_t found = 0;
        for (uint32_t i = 0; i < TOTAL; i++) {
            uint32_t *v = (uint32_t *) hashmap_get(map, &i);
            if (v && *v == i) {
                found++;
            } else {
                printf("Cound not find %u in the map\n", i);
            }
        }
        if (found == TOTAL) {
            printf(
                "Loop %d. All values found. hashmap_put_retries=%u, "
                "hashmap_put_head_fail=%u, hashmap_put_replace_fail=%u\n",
                loops, hashmap_put_retries, hashmap_put_head_fail,
                hashmap_put_replace_fail);
        } else {
            printf("Found %u of %u values. Where are the missing?", found,
                   TOTAL);
        }
    }

    printf("Done. Loops=%d\n", loops);
    return true;
}

bool test_del()
{
    /* keep looping until a CAS retry was needed by hashmap_del */
    uint32_t loops = 0;

    /* make sure test counters are zeroed */
    hashmap_del_fail = hashmap_del_fail_new_head = 0;

    while (hashmap_del_fail == 0 || hashmap_del_fail_new_head == 0) {
        map = hashmap_new(10, cmp_uint32, hash_uint32);

        /* multi-threaded add values */
        if (!mt_del_vals()) {
            printf("test_del() is failing. Can't complete mt_del_vals()");
            return false;
        }
        loops++;

        // check all the list entries
        uint32_t TOTAL = N_THREADS * N_LOOPS, found = 0;
        for (uint32_t i = 0; i < TOTAL; i++) {
            uint32_t *v = hashmap_get(map, &i);
            if (v && *v == i) {
                found++;
            } else {
                printf("Cound not find %u in the hashmap\n", i);
            }
        }
        if (found != TOTAL) {
            printf("test_del() is failing. Not all values found!?");
            return false;
        }
    }
    printf("Done. Needed %u loops\n", loops);
    return true;
}

int main()
{
    free_later_init();

    if (!test_add()) {
        printf("Failed to run multi-threaded addition test.");
        return 1;
    }
    if (!test_del()) {
        printf("Failed to run multi-threaded deletion test.");
        return 2;
    }

    free_later_exit();
    return 0;
}
