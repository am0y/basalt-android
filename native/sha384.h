#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint64_t h[8], bytes; uint8_t block[128]; size_t used; } Sha384;
void sha384_init(Sha384* state);
void sha384_update(Sha384* state, const void* bytes, size_t length);
void sha384_finish(Sha384* state, char output[97]);
