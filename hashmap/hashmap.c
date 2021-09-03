#include "hashmap.h"
#include "free_later.h"

/* TODO: make these variables conditionally built for benchmarking */
/* used for testing CAS-retries in tests */
volatile uint32_t hashmap_put_retries = 0, hashmap_put_replace_fail = 0;
volatile uint32_t hashmap_put_head_fail = 0;
volatile uint32_t hashmap_del_fail = 0, hashmap_del_fail_new_head = 0;

static hashmap_kv_t *create_node_with_malloc(void *opaque,
                                             const void *key,
                                             void *value)
{
    hashmap_kv_t *next = malloc(sizeof *(next));
    next->key = key;
    next->value = value;
    return next;
}

static void destroy_node_later(void *opaque, hashmap_kv_t *node)
{
    /* free all of these later in case other threads are using them */
    free_later((void *) node->key, free);
    free_later(node->value, opaque);
    free_later(node, free);
}

void *hashmap_new(uint32_t n_buckets,
                  uint8_t cmp(const void *x, const void *y),
                  uint64_t hash(const void *key))
{
    hashmap_t *map = calloc(1, sizeof(hashmap_t));
    map->n_buckets = n_buckets;
    map->buckets = calloc(n_buckets, sizeof(hashmap_kv_t *));

    /* keep local reference of the two utility functions */
    map->hash = hash;
    map->cmp = cmp;

    /* custom memory management hook */
    map->opaque = NULL;
    map->create_node = create_node_with_malloc;
    map->destroy_node = destroy_node_later;
    return map;
}

void *hashmap_get(hashmap_t *map, const void *key)
{
    /* hash to convert key to a bucket index where value would be stored */
    uint32_t index = map->hash(key) % map->n_buckets;

    /* walk through the linked list nodes to find any matches */
    for (hashmap_kv_t *n = map->buckets[index]; n; n = n->next) {
        if (map->cmp(n->key, key) == 0)
            return n->value;
    }

    return NULL; /* no matches found */
}

bool hashmap_put(hashmap_t *map, const void *key, void *value)
{
    if (!map)
        return NULL;

    /* hash to convert key to a bucket index where value would be stored */
    uint32_t bucket_index = map->hash(key) % map->n_buckets;

    hashmap_kv_t *kv = NULL, *prev = NULL;

    /* known head and next entry to add to the list */
    hashmap_kv_t *head = NULL, *next = NULL;

    while (true) {
        /* copy the head of the list before checking entries for equality */
        head = map->buckets[bucket_index];

        /* find any existing matches to this key */
        prev = NULL;
        if (head) {
            for (kv = head; kv; kv = kv->next) {
                if (map->cmp(key, kv->key) == 0)
                    break;
                prev = kv;
            }
        }

        if (kv) {      /* if the key exists, update and return it */
            if (!next) /* lazy make the next key-value pair to append */
                next = map->create_node(map->opaque, key, value);

            /* ensure the linked list's existing node chain persists */
            next->next = kv->next;

            /* CAS-update the reference in the previous node */
            if (prev) {
                /* replace this link, assuming it has not changed by another
                 * thread
                 */
                if (__atomic_compare_exchange(&prev->next, &kv, &next, false,
                                              __ATOMIC_SEQ_CST,
                                              __ATOMIC_SEQ_CST)) {
                    /* this node, key and value are never again used by this */
                    map->destroy_node(map->opaque, kv);
                    return true;
                }
                hashmap_put_replace_fail += 1;
            } else { /* no previous link, update the head of the list */
                /* set the head of the list to be whatever this node points to
                 * (NULL or other links)
                 */
                if (__atomic_compare_exchange(&map->buckets[bucket_index], &kv,
                                              &next, false, __ATOMIC_SEQ_CST,
                                              __ATOMIC_SEQ_CST)) {
                    map->destroy_node(map->opaque, kv);
                    return true;
                }

                /* failure means at least one new entry was added, retry the
                 * whole match/del process
                 */
                hashmap_put_head_fail += 1;
            }
        } else {       /* if the key does not exist, try adding it */
            if (!next) /* make the next key-value pair to append */
                next = map->create_node(map->opaque, key, value);
            next->next = NULL;

            if (head) /* make sure the reference to existing nodes is kept */
                next->next = head;

            /* prepend the kv-pair or lazy-make the bucket */
            if (__atomic_compare_exchange(&map->buckets[bucket_index], &head,
                                          &next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST)) {
                __atomic_fetch_add(&map->length, 1, __ATOMIC_SEQ_CST);
                return false;
            }

            /* failure means another thead updated head before this.
             * track the CAS failure for tests -- non-atomic to minimize
             * thread contention
             */
            hashmap_put_retries += 1;
        }
    }
}

bool hashmap_del(hashmap_t *map, const void *key)
{
    if (!map)
        return false;

    uint32_t bucket_index = map->hash(key) % map->n_buckets;

    /* try to find a match, loop in case a delete attempt fails */
    while (true) {
        hashmap_kv_t *match, *prev = NULL;
        for (match = map->buckets[bucket_index]; match; match = match->next) {
            if ((*map->cmp)(key, match->key) == 0)
                break;
            prev = match;
        }

        /* exit if no match was found */
        if (!match)
            return false;

        /* previous means this not the head but a link in the list */
        if (prev) { /* try the delete but fail if another thread did delete */
            if (__atomic_compare_exchange(&prev->next, &match, &match->next,
                                          false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST)) {
                __atomic_fetch_sub(&map->length, 1, __ATOMIC_SEQ_CST);
                map->destroy_node(map->opaque, match);
                return true;
            }
            hashmap_del_fail += 1;
        } else { /* no previous link means this needs to leave empty bucket */
            /* copy the next link in the list (may be NULL) to the head */
            if (__atomic_compare_exchange(&map->buckets[bucket_index], &match,
                                          &match->next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST)) {
                __atomic_fetch_sub(&map->length, 1, __ATOMIC_SEQ_CST);
                map->destroy_node(map->opaque, match);
                return true;
            }

            /* failure means whole match/del process needs another attempt */
            hashmap_del_fail_new_head += 1;
        }
    }

    return false;
}
