// Basalt x86_64 inline hook engine for native 2.744.1081 emulator builds.
#include "hook.h"
#include "build_config.h"
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <android/log.h>

#if defined(__x86_64__)

#define X86_STOLEN_MAX 32

static int page_protection_x86(uint64_t addr, size_t len, int protection) {
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0 || !len) return HOOK_ERR;
    uint64_t mask = (uint64_t)size - 1;
    uint64_t page = addr & ~mask;
    size_t span = (addr + len - page + mask) & ~mask;
    if (mprotect((void*)page, span, protection) != 0) return HOOK_ERR;
    return HOOK_OK;
}

static int stolen_safe_x86(const uint8_t* code, unsigned stolen) {
    for (unsigned i = 0; i < stolen; ++i) {
        uint8_t op = code[i];
        if (op == 0xE8 || op == 0xE9) return 0;            // call/jmp rel32
        if (op == 0xEB || op == 0x70 + 0) if (op >= 0x70 && op <= 0x7F) return 0; // jcc rel8
        if (op == 0x0F && i + 1 < stolen) {
            uint8_t next = code[i + 1];
            if (next >= 0x80 && next <= 0x8F) return 0;    // jcc rel32
        }
        if (op == 0xC2 || op == 0xC3 || op == 0xCA || op == 0xCB || op == 0xEA) return 0;
        // RIP-relative memory forms (mod=00, rm=101) after optional REX prefix.
        if (i + 1 < stolen) {
            uint8_t at = code[i + 1];
            uint8_t modrm = (op >= 0x40 && op <= 0x4F) ? (i + 2 < stolen ? code[i + 2] : 0) : at;
            if (modrm >= 0x40 && (op >= 0x40 && op <= 0x4F ? at : 0) == at) {
                // only inspect when we are positioned at an opcode or REX byte
            }
        }
        if (i + 1 < stolen && (code[i] & 0xF8) == 0x48 && (code[i + 1] & 0xC7) == 0x05) return 0; // REX RIP-rel
        if (i + 2 < stolen && (code[i] & 0xF8) == 0x48 && (code[i + 1] & 0xF8) == 0x48 &&
            (code[i + 2] & 0xC7) == 0x05) return 0; // REX2 RIP-rel
    }
    return 1;
}

int hook_x86_inline(uint64_t target, void* detour, unsigned stolen) {
    uint8_t* original = (uint8_t*)target;
    if (stolen < 13 || stolen > X86_STOLEN_MAX) {
        LOGE("x86 hook: stolen length %u out of range", stolen);
        return HOOK_ERR;
    }
    if (!stolen_safe_x86(original, stolen)) {
        LOGE("x86 target prologue unsafe to steal");
        return HOOK_ERR;
    }
    uint8_t* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { LOGE("x86 hook mmap failed: %s", strerror(errno)); return HOOK_ERR; }
    int at = 0;
    // Save volatile registers (r11 first is not needed; any order works).
    // 9 pushes keep the stack 16-byte aligned for the detour call.
    page[at++] = 0x50; // push rax
    page[at++] = 0x51; // push rcx
    page[at++] = 0x52; // push rdx
    page[at++] = 0x56; // push rsi
    page[at++] = 0x57; // push rdi
    page[at++] = 0x41; page[at++] = 0x50; // push r8
    page[at++] = 0x41; page[at++] = 0x51; // push r9
    page[at++] = 0x41; page[at++] = 0x52; // push r10
    page[at++] = 0x41; page[at++] = 0x53; // push r11
    // Pass the original caller's return address as the optional third argument.
    // Nine pushes saved RDX; the live caller return is at [rsp + 9*8].
    page[at++] = 0x48; page[at++] = 0x8B; page[at++] = 0x54;
    page[at++] = 0x24; page[at++] = 0x48;
    // mov r11, detour; call r11. rdi and rsi still carry (L, from).
    page[at++] = 0x49; page[at++] = 0xBB;
    memcpy(page + at, &detour, 8); at += 8;
    page[at++] = 0x41; page[at++] = 0xFF; page[at++] = 0xD3; // call r11
    // Restore volatile registers.
    page[at++] = 0x41; page[at++] = 0x5B; // pop r11
    page[at++] = 0x41; page[at++] = 0x5A; // pop r10
    page[at++] = 0x41; page[at++] = 0x59; // pop r9
    page[at++] = 0x41; page[at++] = 0x58; // pop r8
    page[at++] = 0x5F; // pop rdi
    page[at++] = 0x5E; // pop rsi
    page[at++] = 0x5A; // pop rdx
    page[at++] = 0x59; // pop rcx
    page[at++] = 0x58; // pop rax
    // Stolen instructions.
    memcpy(page + at, original, stolen); at += stolen;
    // mov r11, target+stolen; jmp r11.
    uint64_t back = target + stolen;
    page[at++] = 0x49; page[at++] = 0xBB;
    memcpy(page + at, &back, 8); at += 8;
    page[at++] = 0x41; page[at++] = 0xFF; page[at++] = 0xE3;

    if (page_protection_x86((uint64_t)page, 4096, PROT_READ | PROT_EXEC) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    if (page_protection_x86(target, stolen, PROT_READ | PROT_WRITE | PROT_EXEC) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    // mov r11, stub; jmp r11 (13 bytes, within the 16 stolen).
    uint8_t patch[13];
    patch[0] = 0x49; patch[1] = 0xBB;
    memcpy(patch + 2, &page, 8);
    patch[10] = 0x41; patch[11] = 0xFF; patch[12] = 0xE3;
    memcpy((void*)target, patch, sizeof(patch));
    if (page_protection_x86(target, stolen, PROT_READ | PROT_EXEC) != HOOK_OK) return HOOK_ERR;
    LOGI("x86 hook installed: target=0x%llx stub=%p detour=%p",
         (unsigned long long)target, page, detour);
    return HOOK_OK;
}

int hook_x86_replace(uint64_t target, void* detour, unsigned stolen, void** trampoline) {
    if (!trampoline || stolen < 13 || stolen > X86_STOLEN_MAX ||
        !stolen_safe_x86((const uint8_t*)target, stolen)) return HOOK_ERR;
    uint8_t* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return HOOK_ERR;
    memcpy(page, (const void*)target, stolen);
    uint64_t back = target + stolen;
    size_t at = stolen;
    page[at++] = 0x49; page[at++] = 0xBB;
    memcpy(page + at, &back, sizeof(back)); at += sizeof(back);
    page[at++] = 0x41; page[at++] = 0xFF; page[at++] = 0xE3;
    if (page_protection_x86((uint64_t)page, 4096, PROT_READ | PROT_EXEC) != HOOK_OK ||
        page_protection_x86(target, stolen, PROT_READ | PROT_WRITE | PROT_EXEC) != HOOK_OK) {
        munmap(page, 4096);
        return HOOK_ERR;
    }
    uint8_t patch[13] = {0x49, 0xBB};
    memcpy(patch + 2, &detour, sizeof(detour));
    patch[10] = 0x41; patch[11] = 0xFF; patch[12] = 0xE3;
    memcpy((void*)target, patch, sizeof(patch));
    if (page_protection_x86(target, stolen, PROT_READ | PROT_EXEC) != HOOK_OK)
        return HOOK_ERR;
    *trampoline = page;
    return HOOK_OK;
}

#endif
