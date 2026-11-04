#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
char* compile_encoded(const char* source, size_t length, size_t* output_size);
#ifdef __cplusplus
}
#endif
