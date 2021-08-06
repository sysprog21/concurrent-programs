/*
 * Hazard pointers are a mechanism for protecting objects in memory from
 * being deleted by other threads while in use. This allows safe lock-free
 * data structures.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define HP_MAX_THREADS 128
#define HP_MAX_HPS 5 /* This is named 'K' in the HP paper */
#define CLPAD (128 / sizeof(uintptr_t))
#define HP_THRESHOLD_R 0 /* This is named 'R' in the HP paper */

/* Maximum number of retired objects per thread */
#define HP_MAX_RETIRED (HP_MAX_THREADS * HP_MAX_HPS)

#define TID_UNKNOWN -1

typedef struct {
    int size;
    uintptr_t *list;
} retirelist_t;

typedef struct list_hp list_hp_t;
typedef void(list_hp_deletefunc_t)(void *);

struct list_hp {
    int max_hps;
    alignas(128) atomic_uintptr_t *hp[HP_MAX_THREADS];
    alignas(128) retirelist_t *rl[HP_MAX_THREADS * CLPAD];
    list_hp_deletefunc_t *deletefunc;
};

static thread_local int tid_v = TID_UNKNOWN;
static atomic_int_fast32_t tid_v_base = ATOMIC_VAR_INIT(0);
static inline int tid(void)
{
    if (tid_v == TID_UNKNOWN) {
        tid_v = atomic_fetch_add(&tid_v_base, 1);
        assert(tid_v < HP_MAX_THREADS);
    }
    return tid_v;
}

/* Create a new hazard pointer array of size 'max_hps' (or a reasonable
 * default value if 'max_hps' is 0). The function 'deletefunc' will be
 * used to delete objects protected by hazard pointers when it becomes
 * safe to retire them.
 */
list_hp_t *list_hp_new(size_t max_hps, list_hp_deletefunc_t *deletefunc)
{
    list_hp_t *hp = aligned_alloc(128, sizeof(*hp));
    assert(hp);

    if (max_hps == 0)
        max_hps = HP_MAX_HPS;

    *hp = (list_hp_t){.max_hps = max_hps, .deletefunc = deletefunc};

    for (int i = 0; i < HP_MAX_THREADS; i++) {
        hp->hp[i] = calloc(CLPAD * 2, sizeof(hp->hp[i][0]));
        hp->rl[i * CLPAD] = calloc(1, sizeof(*hp->rl[0]));
        for (int j = 0; j < hp->max_hps; j++)
            atomic_init(&hp->hp[i][j], 0);
        hp->rl[i * CLPAD]->list = calloc(HP_MAX_RETIRED, sizeof(uintptr_t));
    }

    return hp;
}

/* Destroy a hazard pointer array and clean up all objects protected
 * by hazard pointers.
 */
void list_hp_destroy(list_hp_t *hp)
{
    for (int i = 0; i < HP_MAX_THREADS; i++) {
        free(hp->hp[i]);
        retirelist_t *rl = hp->rl[i * CLPAD];
        for (int j = 0; j < rl->size; j++) {
            void *data = (void *) rl->list[j];
            hp->deletefunc(data);
        }
        free(rl->list);
        free(rl);
    }
    free(hp);
}

/* Clear all hazard pointers in the array for the current thread.
 * Progress condition: wait-free bounded (by max_hps)
 */
void list_hp_clear(list_hp_t *hp)
{
    for (int i = 0; i < hp->max_hps; i++)
        atomic_store_explicit(&hp->hp[tid()][i], 0, memory_order_release);
}

/* This returns the same value that is passed as ptr.
 * Progress condition: wait-free population oblivious.
 */
uintptr_t list_hp_protect_ptr(list_hp_t *hp, int ihp, uintptr_t ptr)
{
    atomic_store(&hp->hp[tid()][ihp], ptr);
    return ptr;
}

/* Same as list_hp_protect_ptr(), but explicitly uses memory_order_release.
 * Progress condition: wait-free population oblivious.
 */
uintptr_t list_hp_protect_release(list_hp_t *hp, int ihp, uintptr_t ptr)
{
    atomic_store_explicit(&hp->hp[tid()][ihp], ptr, memory_order_release);
    return ptr;
}

/* Retire an object that is no longer in use by any thread, calling
 * the delete function that was specified in list_hp_new().
 *
 * Progress condition: wait-free bounded (by the number of threads squared)
 */
void list_hp_retire(list_hp_t *hp, uintptr_t ptr)
{
    retirelist_t *rl = hp->rl[tid() * CLPAD];
    rl->list[rl->size++] = ptr;
    assert(rl->size < HP_MAX_RETIRED);

    if (rl->size < HP_THRESHOLD_R)
        return;

    for (size_t iret = 0; iret < rl->size; iret++) {
        uintptr_t obj = rl->list[iret];
        bool can_delete = true;
        for (int itid = 0; itid < HP_MAX_THREADS && can_delete; itid++) {
            for (int ihp = hp->max_hps - 1; ihp >= 0; ihp--) {
                if (atomic_load(&hp->hp[itid][ihp]) == obj) {
                    can_delete = false;
                    break;
                }
            }
        }

        if (can_delete) {
            size_t bytes = (rl->size - iret) * sizeof(rl->list[0]);
            memmove(&rl->list[iret], &rl->list[iret + 1], bytes);
            rl->size--;
            hp->deletefunc((void *) obj);
        }
    }
}

#include <pthread.h>

#define N_ELEMENTS 128
#define N_THREADS (128 / 2)
#define MAX_THREADS 128

static atomic_uint_fast32_t deletes = 0, inserts = 0;

enum { HP_NEXT = 0, HP_CURR = 1, HP_PREV };

#define is_marked(p) (bool) ((uintptr_t)(p) &0x01)
#define get_marked(p) ((uintptr_t)(p) | (0x01))
#define get_unmarked(p) ((uintptr_t)(p) & (~0x01))

#define get_marked_node(p) ((list_node_t *) get_marked(p))
#define get_unmarked_node(p) ((list_node_t *) get_unmarked(p))

typedef uintptr_t list_key_t;

typedef struct list_node {
    alignas(128) uint32_t magic;
    alignas(128) atomic_uintptr_t next;
    list_key_t key;
} list_node_t;

/* Per list variables */

typedef struct list {
    atomic_uintptr_t head, tail;
    list_hp_t *hp;
} list_t;

#define LIST_MAGIC (0xDEADBEAF)

list_node_t *list_node_new(list_key_t key)
{
    list_node_t *node = aligned_alloc(128, sizeof(*node));
    assert(node);
    *node = (list_node_t){.magic = LIST_MAGIC, .key = key};
    (void) atomic_fetch_add(&inserts, 1);
    return node;
}

void list_node_destroy(list_node_t *node)
{
    if (!node)
        return;
    assert(node->magic == LIST_MAGIC);
    free(node);
    (void) atomic_fetch_add(&deletes, 1);
}

static void __list_node_delete(void *arg)
{
    list_node_t *node = (list_node_t *) arg;
    list_node_destroy(node);
}

static bool __list_find(list_t *list,
                        list_key_t *key,
                        atomic_uintptr_t **par_prev,
                        list_node_t **par_curr,
                        list_node_t **par_next)
{
    atomic_uintptr_t *prev = NULL;
    list_node_t *curr = NULL, *next = NULL;

try_again:
    prev = &list->head;
    curr = (list_node_t *) atomic_load(prev);
    (void) list_hp_protect_ptr(list->hp, HP_CURR, (uintptr_t) curr);
    if (atomic_load(prev) != get_unmarked(curr))
        goto try_again;
    while (true) {
        if (!get_unmarked_node(curr))
            return false;
        next = (list_node_t *) atomic_load(&get_unmarked_node(curr)->next);
        (void) list_hp_protect_ptr(list->hp, HP_NEXT, get_unmarked(next));
        if (atomic_load(&get_unmarked_node(curr)->next) != (uintptr_t) next)
            break;
        if (get_unmarked(next) == atomic_load((atomic_uintptr_t *) &list->tail))
            break;
        if (atomic_load(prev) != get_unmarked(curr))
            goto try_again;
        if (get_unmarked_node(next) == next) {
            if (!(get_unmarked_node(curr)->key < *key)) {
                *par_curr = curr;
                *par_prev = prev;
                *par_next = next;
                return (get_unmarked_node(curr)->key == *key);
            }
            prev = &get_unmarked_node(curr)->next;
            (void) list_hp_protect_release(list->hp, HP_PREV,
                                           get_unmarked(curr));
        } else {
            uintptr_t tmp = get_unmarked(curr);
            if (!atomic_compare_exchange_strong(prev, &tmp, get_unmarked(next)))
                goto try_again;
            list_hp_retire(list->hp, get_unmarked(curr));
        }
        curr = next;
        (void) list_hp_protect_release(list->hp, HP_CURR, get_unmarked(next));
    }
    *par_curr = curr;
    *par_prev = prev;
    *par_next = next;

    return false;
}

bool list_insert(list_t *list, list_key_t key)
{
    list_node_t *curr = NULL, *next = NULL;
    atomic_uintptr_t *prev = NULL;

    list_node_t *node = list_node_new(key);

    while (true) {
        if (__list_find(list, &key, &prev, &curr, &next)) {
            list_node_destroy(node);
            list_hp_clear(list->hp);
            return false;
        }
        atomic_store_explicit(&node->next, (uintptr_t) curr,
                              memory_order_relaxed);
        uintptr_t tmp = get_unmarked(curr);
        if (atomic_compare_exchange_strong(prev, &tmp, (uintptr_t) node)) {
            list_hp_clear(list->hp);
            return true;
        }
    }
}

bool list_delete(list_t *list, list_key_t key)
{
    list_node_t *curr, *next;
    atomic_uintptr_t *prev;
    while (true) {
        if (!__list_find(list, &key, &prev, &curr, &next)) {
            list_hp_clear(list->hp);
            return false;
        }

        uintptr_t tmp = get_unmarked(next);

        if (!atomic_compare_exchange_strong(&curr->next, &tmp,
                                            get_marked(next)))
            continue;

        tmp = get_unmarked(curr);
        if (atomic_compare_exchange_strong(prev, &tmp, get_unmarked(next))) {
            list_hp_clear(list->hp);
            list_hp_retire(list->hp, get_unmarked(curr));
        } else {
            list_hp_clear(list->hp);
        }
        return true;
    }
}

list_t *list_new(void)
{
    list_t *list = calloc(1, sizeof(*list));
    assert(list);
    list_node_t *head = list_node_new(0), *tail = list_node_new(UINTPTR_MAX);
    assert(head), assert(tail);
    list_hp_t *hp = list_hp_new(3, __list_node_delete);

    atomic_init(&head->next, (uintptr_t) tail);
    *list = (list_t){.hp = hp};
    atomic_init(&list->head, (uintptr_t) head);
    atomic_init(&list->tail, (uintptr_t) tail);

    return list;
}

void list_destroy(list_t *list)
{
    assert(list);
    list_node_t *prev = (list_node_t *) atomic_load(&list->head);
    list_node_t *node = (list_node_t *) atomic_load(&prev->next);
    while (node) {
        list_node_destroy(prev);
        prev = node;
        node = (list_node_t *) atomic_load(&prev->next);
    }
    list_node_destroy(prev);
    list_hp_destroy(list->hp);
    free(list);
}

static uintptr_t elements[MAX_THREADS + 1][N_ELEMENTS];

static void *insert_thread(void *arg)
{
    list_t *list = (list_t *) arg;

    for (size_t i = 0; i < N_ELEMENTS; i++)
        (void) list_insert(list, (uintptr_t) &elements[tid()][i]);
    return NULL;
}

static void *delete_thread(void *arg)
{
    list_t *list = (list_t *) arg;

    for (size_t i = 0; i < N_ELEMENTS; i++)
        (void) list_delete(list, (uintptr_t) &elements[tid()][i]);
    return NULL;
}

int main(void)
{
    list_t *list = list_new();

    pthread_t thr[N_THREADS];

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_create(&thr[i], NULL, (i & 1) ? delete_thread : insert_thread,
                       list);

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_join(thr[i], NULL);

    for (size_t i = 0; i < N_ELEMENTS; i++) {
        for (size_t j = 0; j < tid_v_base; j++)
            list_delete(list, (uintptr_t) &elements[j][i]);
    }

    list_destroy(list);

    fprintf(stderr, "inserts = %zu, deletes = %zu\n", atomic_load(&inserts),
            atomic_load(&deletes));

    return 0;
}
