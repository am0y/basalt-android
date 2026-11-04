#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const uint8_t ping[] = {'P', 'I', 'N', 'G'};
    assert(parse_command(ping, sizeof(ping)).type == COMMAND_PING);

    const uint8_t probe[] = {'P', 'R', 'O', 'B', 'E'};
    assert(parse_command(probe, sizeof(probe)).type == COMMAND_PROBE);

    const uint8_t call_probe[] = {'C', 'A', 'L', 'L', '_', 'P', 'R', 'O', 'B', 'E'};
    assert(parse_command(call_probe, sizeof(call_probe)).type == COMMAND_CALL_PROBE);
    const uint8_t context[] = {'C', 'O', 'N', 'T', 'E', 'X', 'T'};
    assert(parse_command(context, sizeof(context)).type == COMMAND_CONTEXT);

    const uint8_t bytecode[] = {'E', 'X', 'E', 'C', '_', 'B', 'I', 'N', '\n', 14, 0, 17, 0};
    struct command command = parse_command(bytecode, sizeof(bytecode));
    assert(command.type == COMMAND_BYTECODE);
    assert(command.size == 4);
    assert(command.data[0] == 14 && command.data[1] == 0);
    assert(command.data[2] == 17 && command.data[3] == 0);

    const uint8_t inspection[] = {'I', 'N', 'S', 'P', 'E', 'C', 'T', '\n', 9, 0, 17, 0};
    command = parse_command(inspection, sizeof(inspection));
    assert(command.type == COMMAND_INSPECT && command.size == 4);
    assert(command.data[0] == 9 && command.data[1] == 0);

    const uint8_t comparison[] = {'C', 'O', 'M', 'P', 'A', 'R', 'E', '\n', 9, 0, 17, 0};
    command = parse_command(comparison, sizeof(comparison));
    assert(command.type == COMMAND_COMPARE && command.size == 4);

    const uint8_t scheduled[] = {'S', 'C', 'H', 'E', 'D', 'U', 'L', 'E', '\n', 9, 0, 17, 0};
    command = parse_command(scheduled, sizeof(scheduled));
    assert(command.type == COMMAND_SCHEDULE && command.size == 4);
    assert(command.data[1] == 0 && command.data[3] == 0);
    assert(parse_command(scheduled, 9).type == COMMAND_INVALID);
    uint8_t* large = calloc(COMMAND_MAX_BYTES + 1, 1);
    assert(large);
    memcpy(large, scheduled, sizeof(scheduled));
    assert(parse_command(large, COMMAND_MAX_BYTES).type == COMMAND_SCHEDULE);
    assert(parse_command(large, COMMAND_MAX_BYTES + 1).type == COMMAND_INVALID);
    free(large);

    assert(parse_command(bytecode, 9).type == COMMAND_INVALID);
    assert(parse_command(ping, 3).type == COMMAND_INVALID);
    assert(parse_command(probe, 4).type == COMMAND_INVALID);
    puts("protocol OK");
    return 0;
}
