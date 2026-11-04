#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define COMMAND_MAX_BYTES (16u * 1024u * 1024u)

enum command_type {
    COMMAND_INVALID,
    COMMAND_PING,
    COMMAND_PROBE,
    COMMAND_CALL_PROBE,
    COMMAND_CONTEXT,
    COMMAND_SCHEDULE,
    COMMAND_INSPECT,
    COMMAND_COMPARE,
    COMMAND_BYTECODE,
};

struct command {
    enum command_type type;
    const uint8_t* data;
    size_t size;
};

static inline struct command parse_command(const uint8_t* bytes, size_t size)
{
    struct command result = {COMMAND_INVALID, NULL, 0};
    if (size == 4 && memcmp(bytes, "PING", 4) == 0)
    {
        result.type = COMMAND_PING;
    }
    else if (size == 5 && memcmp(bytes, "PROBE", 5) == 0)
    {
        result.type = COMMAND_PROBE;
    }
    else if (size == 10 && memcmp(bytes, "CALL_PROBE", 10) == 0)
    {
        result.type = COMMAND_CALL_PROBE;
    }
    else if (size == 7 && memcmp(bytes, "CONTEXT", 7) == 0)
    {
        result.type = COMMAND_CONTEXT;
    }
    else if (size > 9 && size <= COMMAND_MAX_BYTES && memcmp(bytes, "SCHEDULE\n", 9) == 0)
    {
        result.type = COMMAND_SCHEDULE;
        result.data = bytes + 9;
        result.size = size - 9;
    }
    else if (size > 8 && size <= COMMAND_MAX_BYTES && memcmp(bytes, "INSPECT\n", 8) == 0)
    {
        result.type = COMMAND_INSPECT;
        result.data = bytes + 8;
        result.size = size - 8;
    }
    else if (size > 8 && size <= COMMAND_MAX_BYTES && memcmp(bytes, "COMPARE\n", 8) == 0)
    {
        result.type = COMMAND_COMPARE;
        result.data = bytes + 8;
        result.size = size - 8;
    }
    else if (size > 9 && size <= COMMAND_MAX_BYTES && memcmp(bytes, "EXEC_BIN\n", 9) == 0)
    {
        result.type = COMMAND_BYTECODE;
        result.data = bytes + 9;
        result.size = size - 9; // Bytecode contains zero bytes; never use strlen here.
    }
    return result;
}
