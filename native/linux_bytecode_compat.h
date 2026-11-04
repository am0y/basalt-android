#pragma once
#include <stddef.h>

// Lower supported v11-v13 chunks to the v9 serialization. Returns 1 with a
// malloc-owned result, 0 when the original format must be retained, or -1 for
// malformed input/allocation failure. Does not mutate the original bytes.
int linux_bytecode_compat(const char* bytes, size_t length, char** output,
                          size_t* output_length, char* error, size_t capacity);
