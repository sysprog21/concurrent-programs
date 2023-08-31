#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "hash.h"
#include "random.h"
#include "util.h"

/* Maintain thread-independent random seed to prevent race. */
static __thread uint32_t seed = 0;

static uint32_t random_next(void);

void random_init(void)
{
    while (!seed) {
        uint32_t t = (uint32_t) time(NULL);
        seed = t;
        srand48(seed);
    }
}

void random_set_seed(uint32_t seed_)
{
    while (!seed_)
        seed_ = (uint32_t) time(NULL);
    seed = seed_;
    srand48(seed_);
}

uint32_t random_uint32(void)
{
    random_init();
    return random_next();
}

static uint32_t random_next(void)
{
    uint32_t *seedp = &seed;

    *seedp ^= *seedp << 13;
    *seedp ^= *seedp >> 17;
    *seedp ^= *seedp << 5;

    return *seedp;
}
