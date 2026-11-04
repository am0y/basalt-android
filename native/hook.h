// Basalt inline hook engine for arm64. Flushes the full instruction cache.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define HOOK_OK 0
#define HOOK_ERR -1

typedef void (*hook_pre_t)(void *ctx);
typedef long (*hook_wrap_t)(void *ctx, long (*orig)(long, long, long, long));

struct inline_hook {
    uint64_t target;        // address of function start
    uint8_t  orig_bytes[16]; // stolen instructions backup
    uint64_t trampoline;    // Read/execute after construction.
    int      active;
    uint64_t detour;
    size_t allocation_size;
};

// patch target: write abs jump via x16 into first 16 bytes
int hook_arm64_inline(uint64_t target, void *detour);
// The check must not throw. A rejection tail-calls deny with the original stack.
int hook_arm64_guard(uint64_t target, void *check, void *deny);
// restore original bytes
int hook_arm64_restore(struct inline_hook *h);
int hook_arm64_prepare(struct inline_hook* h, uintptr_t target, void* detour,
                       const uint32_t expected[4]);
int hook_arm64_activate(struct inline_hook* h);
#if defined(__x86_64__)
// x86_64 detour: saves volatile registers, calls detour(rdi, rsi), restores,
// executes the 16 stolen bytes and jumps back to target+16.
int hook_x86_inline(uint64_t target, void *detour, unsigned stolen);
int hook_x86_replace(uint64_t target, void *detour, unsigned stolen, void **original);
#endif
// make page RWX
int page_rwx(uint64_t addr, size_t len);
// flush instruction cache (works through native bridge translation)
void flush_icache(uint64_t addr, size_t len);
