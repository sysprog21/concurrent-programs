/* shortcuts */
#define atomic_load(src) __atomic_load_n(src, __ATOMIC_SEQ_CST)
#define atomic_store(dst, val) __atomic_store(dst, val, __ATOMIC_SEQ_CST)
#define atomic_exchange(ptr, val) \
    __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_cas(dst, expected, desired)                                 \
    __atomic_compare_exchange(dst, expected, desired, 0, __ATOMIC_SEQ_CST, \
                              __ATOMIC_SEQ_CST)

#include <stdint.h>

#define LIST_ITER(head, node) \
    for (node = atomic_load(head); node; node = atomic_load(&node->next))

typedef struct __hp {
    uintptr_t ptr;
    struct __hp *next;
} hp_t;

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Allocate a new node with specified value and append to list */
static hp_t *list_append(hp_t **head, uintptr_t ptr)
{
    hp_t *new = calloc(1, sizeof(hp_t));
    if (!new)
        return NULL;

    new->ptr = ptr;
    hp_t *old = atomic_load(head);

    do {
        new->next = old;
    } while (!atomic_cas(head, &old, &new));

    return new;
}

/* Attempt to find an empty node to store value, otherwise append a new node.
 * Returns the node containing the newly added value.
 */
hp_t *list_insert_or_append(hp_t **head, uintptr_t ptr)
{
    hp_t *node;
    bool need_alloc = true;

    LIST_ITER(head, node)
    {
        uintptr_t expected = atomic_load(&node->ptr);
        if (expected == 0 && atomic_cas(&node->ptr, &expected, &ptr)) {
            need_alloc = false;
            break;
        }
    }

    if (need_alloc)
        node = list_append(head, ptr);

    return node;
}

/* Remove a node from the list with the specified value */
bool list_remove(hp_t **head, uintptr_t ptr)
{
    hp_t *node;
    const uintptr_t nullptr = 0;

    LIST_ITER(head, node)
    {
        uintptr_t expected = atomic_load(&node->ptr);
        if (expected == ptr && atomic_cas(&node->ptr, &expected, &nullptr))
            return true;
    }

    return false;
}

/* Returns 1 if the list currently contains an node with the specified value */
bool list_contains(hp_t **head, uintptr_t ptr)
{
    hp_t *node;

    LIST_ITER(head, node)
    {
        if (atomic_load(&node->ptr) == ptr)
            return true;
    }

    return false;
}

/* Compute the size of list */
uint32_t list_size(hp_t *head)
{
    if (!head)
        return 0;
    uint32_t c = 0;
    hp_t *node = head;
    while (node) {
        if (node->ptr)
            c++;
        node = node->next;
    }
    return c;
}

/* Frees all the nodes in a list - NOT THREAD SAFE */
void list_free(hp_t **head)
{
    hp_t *cur = *head;
    while (cur) {
        hp_t *old = cur;
        cur = cur->next;
        free(old);
    }
}

#define DEFER_DEALLOC 1

typedef struct {
    hp_t *pointers;
    // hp_t *retired;
    void (*deallocator)(void *);
} domain_t;

typedef struct {
    hp_t *retired;
    uint32_t r_count;
} wconfig_t;

/* Create a new domain on the heap */
domain_t *domain_new(void (*deallocator)(void *))
{
    domain_t *dom = calloc(1, sizeof(domain_t));
    if (!dom)
        return NULL;

    dom->deallocator = deallocator;
    return dom;
}

/* Free a previously allocated domain */
void domain_free(domain_t *dom)
{
    if (!dom)
        return;

    if (dom->pointers)
        list_free(&dom->pointers);

    free(dom);
}

/* Free wconfig */
void wconfig_free(wconfig_t *wcfig)
{
    if (!wcfig)
        return;

    if (wcfig->retired)
        list_free(&wcfig->retired);

    free(wcfig);
}

/*
 * Load a safe pointer to a shared object. This pointer must be passed to
 * `drop` once it is no longer needed. Returns 0 (NULL) on error.
 */
uintptr_t load(domain_t *dom, const uintptr_t *prot_ptr)
{
    const uintptr_t nullptr = 0;

    while (1) {
        uintptr_t val = atomic_load(prot_ptr);
        hp_t *node = list_insert_or_append(&dom->pointers, val);
        if (!node)
            return 0;

        /* Hazard pointer inserted successfully */
        if (atomic_load(prot_ptr) == val)
            return val;

        /*
         * This pointer is being retired by another thread - remove this hazard
         * pointer and try again. We first try to remove the hazard pointer we
         * just used. If someone else used it to drop the same pointer, we walk
         * the list.
         */
        uintptr_t tmp = val;
        if (!atomic_cas(&node->ptr, &tmp, &nullptr))
            list_remove(&dom->pointers, val);
    }
}

/*
 * Drop a safe pointer to a shared object. This pointer (`safe_val`) must have
 * come from `load`
 */
void drop(domain_t *dom, uintptr_t safe_val)
{
    if (!list_remove(&dom->pointers, safe_val))
        __builtin_unreachable();
}

static void cleanup_ptr(domain_t *dom,
                        wconfig_t *wconfig,
                        uintptr_t ptr,
                        int flags)
{
    if (!list_contains(&dom->pointers, ptr)) { /* deallocate straight away */
        dom->deallocator((void *) ptr);
    } else if (flags & DEFER_DEALLOC) { /* Defer deallocation for later */
        list_insert_or_append(&wconfig->retired, ptr);
        wconfig->r_count += 1;
    } else { /* Spin until all readers are done, then deallocate */
        while (list_contains(&dom->pointers, ptr))
            usleep(10);
        dom->deallocator((void *) ptr);
    }
}

/* Swaps the contents of a shared pointer with a new pointer. The old value will
 * be deallocated by calling the `deallocator` function for the domain, provided
 * when `domain_new` was called. If `flags` is 0, this function will wait
 * until no more references to the old object are held in order to deallocate
 * it. If flags is `DEFER_DEALLOC`, the old object will only be deallocated
 * if there are already no references to it; otherwise the cleanup will be done
 * the next time `cleanup` is called.
 */
void swap(domain_t *dom,
          wconfig_t *wconfig,
          uintptr_t *prot_ptr,
          uintptr_t new_val,
          int flags)
{
    const uintptr_t old_obj = atomic_exchange(prot_ptr, new_val);
    cleanup_ptr(dom, wconfig, old_obj, flags);
}

/* Forces the cleanup of old objects that have not been deallocated yet. Just
 * like `swap`, if `flags` is 0, this function will wait until there are no
 * more references to each object. If `flags` is `DEFER_DEALLOC`, only
 * objects that already have no living references will be deallocated.
 */
void cleanup(domain_t *dom, wconfig_t *wconfig, int flags)
{
    hp_t *node;

    LIST_ITER(&wconfig->retired, node)
    {
        uintptr_t ptr = node->ptr;
        if (!ptr)
            continue;

        if (!list_contains(&dom->pointers, ptr)) {
            /* We can deallocate straight away */
            if (list_remove(&wconfig->retired, ptr))
                dom->deallocator((void *) ptr);
        } else if (!(flags & DEFER_DEALLOC)) {
            /* Spin until all readers are done, then deallocate */
            while (list_contains(&dom->pointers, ptr))
                usleep(10);
            if (list_remove(&wconfig->retired, ptr))
                dom->deallocator((void *) ptr);
        }
    }
}

#include <assert.h>
#include <err.h>
#include <pthread.h>
#include <stdio.h>

#define N_READERS 1
#define N_WRITERS 1
#define N_ITERS 20
#define r_limit_rate 5
#define ARRAY_SIZE(x) sizeof(x) / sizeof(*x)

typedef struct {
    unsigned int v1, v2, v3;
} config_t;

static config_t *shared_config;
static domain_t *config_dom;
static int r_limit = (N_READERS * r_limit_rate) >> 2;

config_t *create_config()
{
    config_t *out = calloc(1, sizeof(config_t));
    if (!out)
        err(EXIT_FAILURE, "calloc");
    return out;
}

void delete_config(config_t *conf)
{
    assert(conf);
    free(conf);
}

wconfig_t *create_wconfig()
{
    wconfig_t *out = calloc(1, sizeof(wconfig_t));
    if (!out)
        err(EXIT_FAILURE, "calloc");
    return out;
}

void copy_config(config_t *conf1, config_t *conf2)
{
    memcpy(conf2, conf1, sizeof(config_t));
}

static void print_config(const char *name, const config_t *conf)
{
    printf("%s : { 0x%08x, 0x%08x, 0x%08x }\n", name, conf->v1, conf->v2,
           conf->v3);
}

void init()
{
    shared_config = create_config();
    config_dom = domain_new(delete_config);
    if (!config_dom)
        err(EXIT_FAILURE, "domain_new");
}

void deinit()
{
    delete_config(shared_config);
    domain_free(config_dom);
}

static void *reader_thread(void *arg)
{
    (void) arg;

    for (int i = 0; i < N_ITERS; ++i) {
        config_t *safe_config =
            (config_t *) load(config_dom, (uintptr_t *) &shared_config);
        if (!safe_config)
            err(EXIT_FAILURE, "load");

        print_config("read config    ", safe_config);
        drop(config_dom, (uintptr_t) safe_config);
    }

    return NULL;
}

static void *writer_thread(void *arg)
{
    (void) arg;
    wconfig_t *wconfig = create_wconfig();
    config_t *cloned_config = create_config();

    for (int i = 0; i < N_ITERS / 2; ++i) {
        config_t *new_config = create_config();
        new_config->v1 = rand();
        new_config->v2 = rand();
        new_config->v3 = rand();
        *cloned_config = *new_config;
        print_config("updating config", new_config);

        swap(config_dom, wconfig, (uintptr_t *) &shared_config,
             (uintptr_t) new_config, 1);
        print_config("updated config ", cloned_config);
        if (wconfig->r_count > r_limit) {
            cleanup(config_dom, wconfig, 1);
            wconfig->r_count = list_size(wconfig->retired);
        }
    }

    cleanup(config_dom, wconfig, 0);
    wconfig_free(wconfig);
    delete_config(cloned_config);
    return NULL;
}

int main()
{
    pthread_t readers[N_READERS], writers[N_WRITERS];

    init();

    /* Start threads */
    for (size_t i = 0; i < ARRAY_SIZE(readers); ++i) {
        if (pthread_create(readers + i, NULL, reader_thread, NULL))
            warn("pthread_create");
    }
    for (size_t i = 0; i < ARRAY_SIZE(writers); ++i) {
        if (pthread_create(writers + i, NULL, writer_thread, NULL))
            warn("pthread_create");
    }

    /* Wait for threads to finish */
    for (size_t i = 0; i < ARRAY_SIZE(readers); ++i) {
        if (pthread_join(readers[i], NULL))
            warn("pthread_join");
    }
    for (size_t i = 0; i < ARRAY_SIZE(writers); ++i) {
        if (pthread_join(writers[i], NULL))
            warn("pthread_join");
    }

    deinit();

    return EXIT_SUCCESS;
}
