#include "process_scope.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const char main_process[] = "com.roblox.client";
    const char auxiliary[] = "com.roblox.client:personaIsolate";
    const char extra_args[] = "com.roblox.client\0--argument";
    assert(executor_is_main_process(main_process, sizeof(main_process)));
    assert(executor_is_main_process(extra_args, sizeof(extra_args)));
    assert(!executor_is_main_process(auxiliary, sizeof(auxiliary)));
    assert(!executor_is_main_process(main_process, sizeof(main_process) - 1));
    assert(!executor_is_main_process("", 1));
    puts("Main-process ownership checks OK");
}
