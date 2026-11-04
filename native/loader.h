#pragma once
#include <stddef.h>
#include <stdint.h>

#define BYTECODE_MAX_BYTES (16u * 1024u * 1024u)

// Accepts only compiler-produced, opcode-encoded Luau v9 in this build's layout.
int bytecode_validate(const uint8_t* bytes, size_t length, char* error, size_t capacity);
int bytecode_load(void* L, const char* name, const uint8_t* bytes, size_t length,
                  const uint64_t* capabilities);
