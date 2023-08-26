#include "util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void abort_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    abort();
}

void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p)
        abort_msg("Out of memory");
    return p;
}
