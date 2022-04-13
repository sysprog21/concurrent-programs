#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "lf_timer.h"

#define EX_HASHSTR(s) #s
#define EX_STR(s) EX_HASHSTR(s)

#define EXPECT(exp)                                                      \
    do {                                                                 \
        if (!(exp))                                                      \
            fprintf(stderr, "FAILURE @ %s:%u; %s\n", __FILE__, __LINE__, \
                    EX_STR(exp)),                                        \
                abort();                                                 \
    } while (0)

static void callback(lf_timer_t tim, lf_tick_t tmo, void *arg)
{
    lf_tick_t tck = lf_timer_tick_get();
    printf("Timer %d expiration %#" PRIx64 " now %#" PRIx64 "\n", tim, tmo,
           tck);
    *(lf_tick_t *) arg = tck;
}

int main(void)
{
    lf_tick_t exp_a = LF_TIMER_TICK_INVALID;
    lf_timer_t tim_a = lf_timer_alloc(callback, &exp_a);
    EXPECT(tim_a != LF_TIMER_NULL);
    EXPECT(lf_timer_set(tim_a, 1));
    EXPECT(!lf_timer_set(tim_a, 1));

    lf_timer_tick_set(0);
    lf_timer_expire();
    EXPECT(exp_a == LF_TIMER_TICK_INVALID);

    lf_timer_tick_set(1);
    lf_timer_expire();
    EXPECT(exp_a == 1);
    EXPECT(lf_timer_set(tim_a, 2));
    EXPECT(lf_timer_reset(tim_a, 3));

    lf_timer_tick_set(2);
    lf_timer_expire();
    EXPECT(exp_a == 1);
    EXPECT(lf_timer_cancel(tim_a));

    lf_timer_tick_set(3);
    lf_timer_expire();
    EXPECT(exp_a == 1);
    EXPECT(!lf_timer_reset(tim_a, UINT64_C(0xFFFFFFFFFFFFFFFE)));
    EXPECT(lf_timer_set(tim_a, UINT64_C(0xFFFFFFFFFFFFFFFE)));
    EXPECT(lf_timer_reset(tim_a, UINT64_C(0xFFFFFFFFFFFFFFFE)));

    lf_timer_expire();
    EXPECT(exp_a == 1);

    lf_timer_tick_set(UINT64_C(0xFFFFFFFFFFFFFFFE));
    lf_timer_expire();
    EXPECT(exp_a == UINT64_C(0xFFFFFFFFFFFFFFFE));

    lf_timer_free(tim_a);

    printf("timer tests complete\n");
    return 0;
}
