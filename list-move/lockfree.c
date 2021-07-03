#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "bench.h"

#define QSBR_N_EPOCHS 3
#define QSBR_PADDING 16
#define QSBR_FREELIST_SIZE 32768

typedef struct {
    int epoch;
    bool in_critical;
    long qsbr_padding[QSBR_PADDING];
    size_t freelist_count[QSBR_N_EPOCHS];
    void *freelist[QSBR_N_EPOCHS][QSBR_FREELIST_SIZE];
} qsbr_pthread_data_t;

#define CAS(addr, oldv, newv)                                              \
    __atomic_compare_exchange((addr), &(oldv), &(newv), 0, __ATOMIC_RELAXED, \
                              __ATOMIC_RELAXED)
#define FETCH_AND_ADD(addr, v) __atomic_fetch_add((addr), (v), __ATOMIC_RELAXED)
#define MEM_BARRIER() __sync_synchronize()

static long n_threads = 0;
#define QSBR_MAX_THREADS 448
static qsbr_pthread_data_t *threads[QSBR_MAX_THREADS] = {0};

static volatile pthread_spinlock_t update_lock
    __attribute__((__aligned__(CACHE_LINE)));

static volatile int global_epoch __attribute__((__aligned__(CACHE_LINE)));
static void qsbr_init(void)
{
    global_epoch = 1;
    pthread_spin_init(&update_lock, PTHREAD_PROCESS_SHARED);
}

static void qsbr_pthread_init(qsbr_pthread_data_t *qsbr_data)
{
    qsbr_data->epoch = 0;
    qsbr_data->in_critical = true;

    for (int i = 0; i < QSBR_N_EPOCHS; i++)
        qsbr_data->freelist_count[i] = 0;

    threads[FETCH_AND_ADD(&n_threads, 1)] = qsbr_data;
}

static inline void qsbr_free(qsbr_pthread_data_t *qsbr_data, int epoch)
{
    MEM_BARRIER();

    for (int i = 0; i < qsbr_data->freelist_count[epoch]; i++)
        free(qsbr_data->freelist[epoch][i]);
    qsbr_data->freelist_count[epoch] = 0;
}

static bool qsbr_update_epoch()
{
    if (!pthread_spin_trylock(&update_lock))
        return false;

    int cur_epoch = global_epoch;
    for (int i = 0; i < n_threads; i++) {
        if (threads[i]->in_critical && threads[i]->epoch != cur_epoch) {
            pthread_spin_unlock(&update_lock);
            return false;
        }
    }

    global_epoch = (cur_epoch + 1) % QSBR_N_EPOCHS;

    pthread_spin_unlock(&update_lock);

    return true;
}

static void qsbr_quiescent_state(qsbr_pthread_data_t *qsbr_data)
{
    int epoch = global_epoch;
    if (qsbr_data->epoch != epoch) {
        qsbr_free(qsbr_data, epoch);
        qsbr_data->epoch = epoch;
    } else {
        qsbr_data->in_critical = false;
        if (qsbr_update_epoch()) {
            qsbr_data->in_critical = true;
            MEM_BARRIER();
            epoch = global_epoch;
            if (qsbr_data->epoch != epoch) {
                qsbr_free(qsbr_data, epoch);
                qsbr_data->epoch = epoch;
            }
            return;
        }
        qsbr_data->in_critical = true;
        MEM_BARRIER();
    }
}

static void qsbr_free_ptr(void *ptr, qsbr_pthread_data_t *qsbr_data)
{
    assert(qsbr_data->freelist_count[qsbr_data->epoch] < QSBR_FREELIST_SIZE);

    qsbr_data->freelist[qsbr_data->epoch]
                       [(qsbr_data->freelist_count[qsbr_data->epoch])++] = ptr;
}

typedef struct lf_list_slot {
    unsigned long epoch;
    struct node *next;
    struct lf_list_slot *slot_next;
    struct lf_list_record *rec;
} lf_list_slot_t;

#define ENTRIES_PER_TASK 3

typedef struct lf_list_record {
    unsigned long epoch;
    struct lf_list_record *rec_next;
    int count;
    struct node *nodes[ENTRIES_PER_TASK];
    struct lf_list_slot *slots[ENTRIES_PER_TASK];
} lf_list_record_t;

typedef struct node {
    int val;
    lf_list_slot_t *slots;
} node_t;

typedef struct {
    node_t *head[2];
} lf_list_t;

typedef struct {
    lf_list_record_t *rec;
    unsigned long epoch;
    lf_list_record_t *new_rec;
    unsigned long count;
    qsbr_pthread_data_t *qsbr_data;
} lf_list_pthread_data_t;

enum { INDIRECT_EPOCH = 0, INACTIVE_EPOCH, STARTING_EPOCH };

static volatile lf_list_record_t *committed_rec = NULL;

#define QSBR_PERIOD 100
static inline void list_maybe_quiescent(lf_list_pthread_data_t *lf_list_data)
{
    lf_list_data->count++;
    if (lf_list_data->count % QSBR_PERIOD == 0) {
        lf_list_record_t *rec;
        do {
            rec = *(lf_list_record_t **) &committed_rec;
        } while (rec->epoch < lf_list_data->epoch &&
                 !CAS(&committed_rec, rec, lf_list_data->rec));
        qsbr_quiescent_state(lf_list_data->qsbr_data);
    }
}

static inline unsigned long read_slot_epoch(lf_list_slot_t *slot)
{
    if (slot->epoch == INDIRECT_EPOCH)
        return slot->rec->epoch;
    return slot->epoch;
}

static inline void lf_list_free_later(void *ptr,
                                      lf_list_pthread_data_t *lf_list_data)
{
    qsbr_free_ptr(ptr, lf_list_data->qsbr_data);
}

static inline void lf_list_free_slots_later(
    lf_list_slot_t *slot,
    lf_list_pthread_data_t *lf_list_data)
{
    for (struct lf_list_slot *it_slot = slot; it_slot;
         it_slot = it_slot->slot_next) {
        lf_list_free_later(it_slot, lf_list_data);
        if (read_slot_epoch(it_slot) >= STARTING_EPOCH)
            break;
    }
}

static inline void add_slot(node_t *node,
                            node_t *next,
                            lf_list_pthread_data_t *lf_list_data)
{
    lf_list_slot_t *old_slot;
    lf_list_record_t *rec = lf_list_data->new_rec;

    lf_list_slot_t *slot = malloc(sizeof(lf_list_slot_t));
    slot->epoch = INDIRECT_EPOCH;
    slot->rec = rec;
    slot->next = next;
    rec->nodes[rec->count] = node;
    rec->slots[rec->count] = slot;
    rec->count++;
    do {
        old_slot = node->slots;
        slot->slot_next = old_slot;
        MEM_BARRIER();
    } while (!CAS(&(node->slots), old_slot, slot));
}

static inline lf_list_slot_t *read_slot(node_t *node,
                                        lf_list_pthread_data_t *lf_list_data)
{
    unsigned long epoch = lf_list_data->epoch;
    lf_list_slot_t *it_slot;
    for (it_slot = node->slots; it_slot; it_slot = it_slot->slot_next) {
        unsigned long slot_epoch = read_slot_epoch(it_slot);
        if (slot_epoch >= STARTING_EPOCH && slot_epoch <= epoch)
            break;
    }

    assert(it_slot->next != node);
    return it_slot;
}

static inline node_t *lf_list_get_next(node_t *node,
                                       lf_list_pthread_data_t *lf_list_data)
{
    return read_slot(node, lf_list_data)->next;
}

static inline void lf_list_set_read_epoch(lf_list_pthread_data_t *lf_list_data)
{
    lf_list_record_t *next = lf_list_data->rec;
    lf_list_record_t *rec = next;
    unsigned long epoch = next->epoch;

    while ((next = next->rec_next)) {
        rec = next;
        epoch++;
        if ((*(volatile unsigned long *) &next->epoch == INACTIVE_EPOCH))
            next->epoch = epoch;
    }
    lf_list_data->rec = rec;
    lf_list_data->epoch = epoch;
}

static inline void lf_list_write_cs_enter(lf_list_pthread_data_t *lf_list_data)
{
    lf_list_record_t *rec;
    lf_list_data->new_rec = rec = malloc(sizeof(lf_list_record_t));
    assert(rec);

    rec->epoch = INACTIVE_EPOCH;
    rec->count = 0;
    rec->rec_next = NULL;
    lf_list_set_read_epoch(lf_list_data);
}

static int lf_list_write_cs_exit(lf_list_pthread_data_t *lf_list_data)
{
    lf_list_record_t *new_rec = lf_list_data->new_rec;
    int ret = 0;
    if (new_rec->count == 0) {
        free(new_rec);
        goto out;
    }

    lf_list_record_t *it_rec = lf_list_data->rec;
    unsigned long epoch = lf_list_data->epoch + 1;
    while (1) {
        while (it_rec->rec_next) {
            it_rec = it_rec->rec_next;
            if (new_rec->nodes[0] == it_rec->nodes[0] ||
                new_rec->nodes[1] == it_rec->nodes[0] ||
                new_rec->nodes[2] == it_rec->nodes[0] ||
                new_rec->nodes[0] == it_rec->nodes[1] ||
                new_rec->nodes[1] == it_rec->nodes[1] ||
                new_rec->nodes[2] == it_rec->nodes[1] ||
                new_rec->nodes[0] == it_rec->nodes[2] ||
                new_rec->nodes[1] == it_rec->nodes[2] ||
                new_rec->nodes[2] == it_rec->nodes[2]) {
                ret = 1;
                goto out;
            }
            epoch++;
            if ((*(volatile unsigned long *) &it_rec->epoch == INACTIVE_EPOCH))
                it_rec->epoch = epoch;
        }

        const void *nullptr = NULL;
        if (CAS(&it_rec->rec_next, nullptr, new_rec)) {
            new_rec->epoch = epoch;
            new_rec->slots[0]->epoch = epoch;
            new_rec->slots[1]->epoch = epoch;
            new_rec->slots[2]->epoch = epoch;
            lf_list_free_slots_later(new_rec->slots[0]->slot_next,
                                     lf_list_data);
            lf_list_free_slots_later(new_rec->slots[1]->slot_next,
                                     lf_list_data);
            lf_list_free_slots_later(new_rec->slots[2]->slot_next,
                                     lf_list_data);
            lf_list_free_later(it_rec, lf_list_data);
            lf_list_data->rec = new_rec;
            lf_list_data->epoch = epoch;
            break;
        }
    }

out:
    if (ret) {
        new_rec->slots[0]->epoch = INACTIVE_EPOCH;
        new_rec->slots[1]->epoch = INACTIVE_EPOCH;
        new_rec->slots[2]->epoch = INACTIVE_EPOCH;
        lf_list_free_later(new_rec, lf_list_data);
        lf_list_data->new_rec = new_rec = malloc(sizeof(lf_list_record_t));
        assert(new_rec);
        new_rec->epoch = INACTIVE_EPOCH;
        new_rec->count = 0;
        new_rec->rec_next = NULL;
        lf_list_set_read_epoch(lf_list_data);
    } else {
        lf_list_data->new_rec = NULL;
    }

    return ret;
}

pthread_data_t *alloc_pthread_data(void)
{
    size_t pthread_size = sizeof(pthread_data_t);
    pthread_size = CACHE_ALIGN(pthread_size);
    size_t lf_list_size = sizeof(lf_list_pthread_data_t);
    lf_list_size = CACHE_ALIGN(lf_list_size);
    size_t qsbr_size = sizeof(qsbr_pthread_data_t);
    qsbr_size = CACHE_ALIGN(qsbr_size);

    pthread_data_t *d = malloc(pthread_size + lf_list_size + qsbr_size);
    if (d) {
        d->ds_data = ((void *) d) + pthread_size;
        ((lf_list_pthread_data_t *) d->ds_data)->qsbr_data =
            ((void *) d) + pthread_size + lf_list_size;
    }

    return d;
}

void free_pthread_data(pthread_data_t *d)
{
    /* free QSBR freelist */
    free(d);
}

void *list_global_init(int size, int value_range)
{
    node_t *node[2];
    lf_list_slot_t *slot[2];

    lf_list_record_t *rec = malloc(sizeof(lf_list_record_t));
    if (!rec)
        return NULL;
    rec->epoch = STARTING_EPOCH;
    rec->rec_next = NULL;
    rec->count = 0;
    committed_rec = rec;

    lf_list_t *list = malloc(sizeof(lf_list_t));
    if (!list)
        return NULL;
    list->head[0] = malloc(sizeof(node_t));
    list->head[1] = malloc(sizeof(node_t));
    node[0] = list->head[0];
    node[1] = list->head[1];
    if (!node[0] || !node[1])
        return NULL;
    node[0]->val = INT_MIN;
    node[0]->slots = malloc(sizeof(lf_list_slot_t));
    slot[0] = node[0]->slots;
    if (!slot[0])
        return NULL;
    slot[0]->epoch = STARTING_EPOCH;
    slot[0]->slot_next = NULL;
    slot[0]->rec = NULL;
    node[1]->val = INT_MIN;
    node[1]->slots = malloc(sizeof(lf_list_slot_t));
    slot[1] = node[1]->slots;
    if (!slot[1])
        return NULL;
    slot[1]->epoch = STARTING_EPOCH;
    slot[1]->slot_next = NULL;
    slot[1]->rec = NULL;

    for (int i = 0; i < value_range; i += value_range / size) {
        slot[0]->next = malloc(sizeof(node_t));
        if (!slot[0]->next)
            return NULL;
        node[0] = slot[0]->next;
        node[0]->val = i;
        node[0]->slots = malloc(sizeof(lf_list_slot_t));
        slot[0] = node[0]->slots;
        if (!slot[0])
            return NULL;
        slot[0]->epoch = STARTING_EPOCH;
        slot[0]->slot_next = NULL;
        slot[0]->rec = NULL;
        slot[1]->next = malloc(sizeof(node_t));
        if (!slot[1]->next)
            return NULL;
        node[1] = slot[1]->next;
        node[1]->val = i + 1;
        node[1]->slots = malloc(sizeof(lf_list_slot_t));
        slot[1] = node[1]->slots;
        if (!slot[1])
            return NULL;
        slot[1]->epoch = STARTING_EPOCH;
        slot[1]->slot_next = NULL;
        slot[1]->rec = NULL;
    }

    slot[0]->next = malloc(sizeof(node_t));
    if (!slot[0]->next)
        return NULL;
    node[0] = slot[0]->next;
    node[0]->val = INT_MAX;
    node[0]->slots = malloc(sizeof(lf_list_slot_t));
    slot[0] = node[0]->slots;
    if (!slot[0])
        return NULL;
    slot[0]->epoch = STARTING_EPOCH;
    slot[0]->slot_next = NULL;
    slot[0]->rec = NULL;
    slot[0]->next = NULL;
    slot[1]->next = malloc(sizeof(node_t));
    if (!slot[1]->next)
        return NULL;
    node[1] = slot[1]->next;
    node[1]->val = INT_MAX;
    node[1]->slots = malloc(sizeof(lf_list_slot_t));
    slot[1] = node[1]->slots;
    if (!slot[1])
        return NULL;
    slot[1]->epoch = STARTING_EPOCH;
    slot[1]->slot_next = NULL;
    slot[1]->rec = NULL;
    slot[1]->next = NULL;

    qsbr_init();

    return list;
}

int list_thread_init(pthread_data_t *data)
{
    lf_list_pthread_data_t *lf_list_data =
        (lf_list_pthread_data_t *) data->ds_data;

    lf_list_data->rec = *(struct lf_list_record **) &committed_rec;
    lf_list_data->epoch = lf_list_data->rec->epoch;
    lf_list_data->new_rec = NULL;

    lf_list_data->count = 0;
    qsbr_pthread_init(lf_list_data->qsbr_data);

    return 0;
}

void list_global_exit(void *list)
{
    /* TODO: free list->head */
}

int list_move(int key, pthread_data_t *data, int from)
{
    lf_list_t *list = (lf_list_t *) data->list;
    lf_list_pthread_data_t *lf_list_data =
        (lf_list_pthread_data_t *) data->ds_data;
    int ret, val;

    lf_list_write_cs_enter(lf_list_data);
    do {
        node_t *prev_src = list->head[from], *next_dst, *cur;
        while (1) {
            cur = lf_list_get_next(prev_src, lf_list_data);
            val = cur->val;
            if (val >= key)
                break;
            prev_src = cur;
        }
        ret = (val == key);
        if (!ret)
            goto out;
        node_t *next_src = lf_list_get_next(cur, lf_list_data);
        node_t *prev_dst = list->head[1 - from];
        while (1) {
            next_dst = lf_list_get_next(prev_dst, lf_list_data);
            val = next_dst->val;
            if (val >= key)
                break;
            prev_dst = next_dst;
        }
        ret = (val != key);
        if (!ret)
            goto out;
        add_slot(prev_src, next_src, lf_list_data);
        add_slot(prev_dst, cur, lf_list_data);
        add_slot(cur, next_dst, lf_list_data);
    out:;
    } while (lf_list_write_cs_exit(lf_list_data));

    list_maybe_quiescent(lf_list_data);

    return ret;
}
