#pragma once
#include <stddef.h>
#include <string.h>

static inline int executor_is_main_process(const char* command, size_t length)
{
    static const char expected[] = "com.roblox.client";
    return length >= sizeof(expected) && !memcmp(command, expected, sizeof(expected));
}
