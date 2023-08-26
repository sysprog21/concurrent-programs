#include <stdio.h>
#include <string.h>

#include "expect.h"
#include "seqlock.h"

int main(void)
{
    seqlock_t sync;
    seqlock_t s;
    char data[24] = {0};
    data[23] = (char) 255;
    seqlock_init(&sync);
    s = seqlock_acquire_rd(&sync);
    EXPECT(seqlock_release_rd(&sync, s) == true);
    s = seqlock_acquire_rd(&sync);
    seqlock_write(&sync, "Mary had a little lamb", data, 23);
    EXPECT(seqlock_release_rd(&sync, s) == false);
    EXPECT(strncmp(data, "Mary had a little lamb", 23) == 0);
    EXPECT(data[23] == (char) 255);

    return 0;
}
