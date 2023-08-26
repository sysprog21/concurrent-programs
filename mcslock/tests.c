#include <stdio.h>

#include "mcslock.h"

int main(void)
{
    mcslock_t lock;
    mcsnode_t node;
    mcslock_init(&lock);
    mcslock_acquire(&lock, &node);
    mcslock_release(&lock, &node);
    mcslock_acquire(&lock, &node);
    mcslock_release(&lock, &node);

    return 0;
}
