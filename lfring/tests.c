#include <stdio.h>
#include <stdlib.h>

#include "lfring.h"

#define EX_HASHSTR(s) #s
#define EX_STR(s) EX_HASHSTR(s)

#define EXPECT(exp)                                                      \
    {                                                                    \
        if (!(exp))                                                      \
            fprintf(stderr, "FAILURE @ %s:%u; %s\n", __FILE__, __LINE__, \
                    EX_STR(exp)),                                        \
                abort();                                                 \
    }

static void test_ringbuffer(uint32_t flags)
{
    void *vec[4];
    uint32_t ret;
    uint32_t idx;

    lfring_t *rb = lfring_alloc(2, flags);
    EXPECT(rb != NULL);

    ret = lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 0);
    ret = lfring_enqueue(rb, (void *[]){(void *) 1}, 1);
    EXPECT(ret == 1);

    ret = lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 0);
    EXPECT(vec[0] == (void *) 1);

    ret = lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 0);

    ret = lfring_enqueue(rb, (void *[]){(void *) 2, (void *) 3, (void *) 4}, 3);
    EXPECT(ret == 2);

    ret = lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 1);
    EXPECT(vec[0] == (void *) 2);

    ret = lfring_dequeue(rb, vec, 4, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 2);
    EXPECT(vec[0] == (void *) 3);

    lfring_free(rb);
}

int main(void)
{
    printf("testing MPMC lock-free ring\n");
    test_ringbuffer(LFRING_FLAG_MP | LFRING_FLAG_MC);

    printf("testing MPSC lock-free ring\n");
    test_ringbuffer(LFRING_FLAG_MP | LFRING_FLAG_SC);

    printf("testing SPMC lock-free ring\n");
    test_ringbuffer(LFRING_FLAG_SP | LFRING_FLAG_MC);

    printf("testing SPSC lock-free ring\n");
    test_ringbuffer(LFRING_FLAG_SP | LFRING_FLAG_SC);

    return 0;
}
