#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util.h"

/* A Universally Unique IDentifier (UUID) compliant with RFC 4122.
 *
 * Each of the parts is stored in host byte order, but the parts themselves are
 * ordered from left to right.  That is, (parts[0] >> 24) is the first 8 bits
 * of the UUID when output in the standard form, and (parts[3] & 0xff) is the
 * final 8 bits.
 */
struct uuid {
    uint32_t parts[4];
};

static inline uint32_t hash_rot(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

/* Murmurhash by Austin Appleby,
 * from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
 */
static inline uint32_t mhash_add__(uint32_t hash, uint32_t data)
{
    /* zero-valued 'data' will not change the 'hash' value */
    if (!data)
        return hash;

    data *= 0xcc9e2d51;
    data = hash_rot(data, 15);
    data *= 0x1b873593;
    return hash ^ data;
}

static inline uint32_t mhash_add(uint32_t hash, uint32_t data)
{
    hash = mhash_add__(hash, data);
    hash = hash_rot(hash, 13);
    return hash * 5 + 0xe6546b64;
}

static inline uint32_t mhash_finish(uint32_t hash)
{
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return hash;
}

static inline uint32_t hash_add(uint32_t hash, uint32_t data)
{
    return mhash_add(hash, data);
}

static inline uint32_t hash_finish(uint32_t hash, uint32_t final)
{
    return mhash_finish(hash ^ final);
}

static inline uint32_t hash_2words(uint32_t x, uint32_t y)
{
    return hash_finish(hash_add(hash_add(x, 0), y), 8);
}

static inline uint32_t hash_int(uint32_t x, uint32_t basis)
{
    return hash_2words(x, basis);
}
