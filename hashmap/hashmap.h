/* Lock-Free Hashmap
 *
 * This implementation is thread-safe and lock-free. It will perform well as
 * long as the initial bucket size is large enough.
 */

#ifndef _HASHMAP_H_
#define _HASHMAP_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* links in the linked lists that each bucket uses */
typedef struct hashmap_keyval {
    struct hashmap_keyval *next;
    const void *key;
    void *value;
} hashmap_kv_t;

/* main hashmap struct with buckets of linked lists */
typedef struct {
    hashmap_kv_t **buckets;
    uint32_t n_buckets;

    uint32_t length; /* total count of entries */

    /* pointer to the hash and comparison functions */
    uint64_t (*hash)(const void *key);
    uint8_t (*cmp)(const void *x, const void *y);

    /* custom memory management of internal linked lists */
    void *opaque;
    hashmap_kv_t *(*create_node)(void *opaque, const void *key, void *data);
    void (*destroy_node)(void *opaque, hashmap_kv_t *node);
} hashmap_t;

/* Create and initialize a new hashmap */
void *hashmap_new(uint32_t hint,
                  uint8_t cmp(const void *x, const void *y),
                  uint64_t hash(const void *key));

/* Return a value mapped to key or NULL, if no entry exists for the given */
void *hashmap_get(hashmap_t *map, const void *key);

/* Put the given key-value pair in the map.
 * @return true if an existing matching key was replaced.
 */
bool hashmap_put(hashmap_t *map, const void *key, void *value);

/* Remove the given key-value pair in the map.
 * @return true if a key was found.
 * This operation is guaranteed to return true just once, if multiple threads
 * are attempting to delete the same key.
 */
bool hashmap_del(hashmap_t *map, const void *key);

#endif
