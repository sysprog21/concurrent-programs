#pragma once

#include <stdio.h>
#include <stdlib.h>

#define EX_HASHSTR(s) #s
#define EX_STR(s) EX_HASHSTR(s)

#define EXPECT(exp)                                                      \
    {                                                                    \
        if (!(exp))                                                      \
            fprintf(stderr, "FAILURE @ %s:%u; %s\n", __FILE__, __LINE__, \
                    EX_STR(exp)),                                        \
                abort();                                                 \
    }
