#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void random_init(void);
void random_set_seed(uint32_t);

uint32_t random_uint32(void);
