#pragma once
#include <stdint.h>

enum embedded_code { EMBEDDED_SCHEDULER, EMBEDDED_EDITOR };

int load_embedded_bytecode(void* L, const char* name, enum embedded_code code,
                           const uint64_t* capabilities);
