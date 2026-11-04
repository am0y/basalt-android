// Basalt arm64 inline hook engine. Version 3 emits trampolines directly.
#include "hook.h"
#include "build_config.h"
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

static int page_protection(uint64_t addr, size_t len, int protection) {
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0 || !len || addr > UINT64_MAX - len - (size_t)size) return HOOK_ERR;
    uint64_t mask = (uint64_t)size - 1;
    uint64_t page = addr & ~mask;
    size_t span = (addr + len - page + mask) & ~mask;
    if (mprotect((void*)page, span, protection) != 0) {
        LOGE("mprotect failed: %s", strerror(errno));
        return HOOK_ERR;
    }
    return HOOK_OK;
}

int page_rwx(uint64_t addr, size_t len) {
    return page_protection(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC);
}

void flush_icache(uint64_t addr, size_t len) {
    __builtin___clear_cache((char*)addr, (char*)(addr + len));
}

static inline uint32_t stp_pre(int rt, int rt2) {
    return 0xA9BF0000u | ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}
static inline uint32_t ldp_post(int rt, int rt2) {
    return 0xA8C10000u | ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}
static inline uint32_t mov_reg(int rd, int rs) {
    return 0xAA0003E0u | ((uint32_t)rs << 16) | (uint32_t)rd;
}
static void write_abs_jump(uint64_t at, uint64_t dest) {
    uint32_t ldr = 0x58000050; // LDR X16, #8
    uint32_t br  = 0xD61F0200; // BR X16
    memcpy((void*)at, &ldr, 4);
    memcpy((void*)(at + 4), &br, 4);
    memcpy((void*)(at + 8), &dest, 8);
}

static int install_jump(uint64_t target, uint64_t detour, const uint8_t original[16]) {
    if (page_rwx(target, 16) != HOOK_OK) return HOOK_ERR;
    write_abs_jump(target, detour);
    flush_icache(target, 16);
    if (page_protection(target, 16, PROT_READ | PROT_EXEC) == HOOK_OK) return HOOK_OK;

    memcpy((void*)target, original, 16);
    flush_icache(target, 16);
    page_protection(target, 16, PROT_READ | PROT_EXEC);
    return HOOK_ERR;
}

static int stolen_ok(const uint8_t *b) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t w; memcpy(&w, b + i * 4, 4);
        if ((w & 0x1f000000u) == 0x10000000u || /* ADR / ADRP */
            (w & 0x7c000000u) == 0x14000000u || /* B / BL */
            (w & 0xff000010u) == 0x54000000u || /* B.cond */
            (w & 0x7e000000u) == 0x34000000u || /* CBZ / CBNZ */
            (w & 0x7e000000u) == 0x36000000u || /* TBZ / TBNZ */
            (w & 0x3b000000u) == 0x18000000u) return 0; /* literal load */
    }
    return 1;
}

int hook_arm64_prepare(struct inline_hook* h, uintptr_t target, void* detour,
                       const uint32_t expected[4]) {
    if (!h || h->active || h->trampoline || !target || (target & 3) || !detour || !expected ||
        memcmp((void*)target, expected, 16) || !stolen_ok((void*)target)) return HOOK_ERR;
    long allocation = sysconf(_SC_PAGESIZE);
    if (allocation < 32) return HOOK_ERR;
    uint8_t* page = mmap(NULL, (size_t)allocation, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return HOOK_ERR;
    memcpy(page, (void*)target, 16);
    write_abs_jump((uintptr_t)page + 16, target + 16);
    flush_icache((uintptr_t)page, 32);
    if (page_protection((uintptr_t)page, (size_t)allocation, PROT_READ | PROT_EXEC) != HOOK_OK) {
        munmap(page, (size_t)allocation); return HOOK_ERR;
    }
    h->target = target; h->detour = (uintptr_t)detour;
    memcpy(h->orig_bytes, (void*)target, 16);
    h->trampoline = (uintptr_t)page; h->allocation_size = (size_t)allocation;
    return HOOK_OK;
}

int hook_arm64_activate(struct inline_hook* h) {
    if (!h || !h->trampoline || h->active ||
        memcmp((void*)h->target, h->orig_bytes, 16)) return HOOK_ERR;
    if (install_jump(h->target, h->detour, h->orig_bytes) != HOOK_OK) return HOOK_ERR;
    h->active = 1;
    return HOOK_OK;
}

/* Rollback before these entry points can be used; not a live unhook API. */
int hook_arm64_restore(struct inline_hook* h) {
    if (!h) return HOOK_ERR;
    if (h->active) {
        if (page_rwx(h->target, 16) != HOOK_OK) return HOOK_ERR;
        memcpy((void*)h->target, h->orig_bytes, 16);
        flush_icache(h->target, 16);
        h->active = 0;
        if (page_protection(h->target, 16, PROT_READ | PROT_EXEC) != HOOK_OK) return HOOK_ERR;
    }
    if (h->trampoline) munmap((void*)h->trampoline, h->allocation_size);
    memset(h, 0, sizeof(*h));
    return HOOK_OK;
}

int hook_arm64_inline(uint64_t target, void *detour) {
    uint8_t *orig = (uint8_t*)target;
    uint8_t original[16];
    if (!stolen_ok(orig)) {
        LOGE("target prologue unsafe to steal");
        return HOOK_ERR;
    }
    uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { LOGE("mmap failed: %s", strerror(errno)); return HOOK_ERR; }

    uint32_t *code = (uint32_t*)page;
    int i = 0;
    // save: x29/x30 then x0..x17
    code[i++] = stp_pre(29, 30);
    for (int r = 0; r < 18; r += 2) code[i++] = stp_pre(r, r + 1);
    // The original x1 is the VM stack slot passed to luaD_callint. The
    // callback can grow that stack, so pass x1 through and save its rebased
    // return value into the register frame before restoring it.
    code[i++] = 0xF94047E1; // ldr x1, [sp, #136]
    // Call the detour through x17. The mmap page can be far from the library.
    code[i++] = 0x58000071;              // ldr x17, #8
    code[i++] = 0xD63F0220;              // blr x17
    code[i++] = 0x14000003;              // b +12 (skip the quad)
    uint64_t d = (uint64_t)detour;
    memcpy(&code[i], &d, 8);
    i += 2;
    code[i++] = 0xF90047E0; // str x0, [sp, #136]
    // restore x0..x17 then x29/x30
    for (int r = 16; r >= 0; r -= 2) code[i++] = ldp_post(r, r + 1);
    code[i++] = ldp_post(29, 30);
    // stolen instructions
    memcpy(&code[i], orig, 16);
    i += 4;
    // abs jump back to target+16
    code[i++] = 0x58000050; // ldr x16, #8
    code[i++] = 0xD61F0200; // br x16
    uint64_t back = target + 16;
    memcpy(&code[i], &back, 8);
    i += 2;

    flush_icache((uint64_t)page, 4096);
    if (page_protection((uint64_t)page, 4096, PROT_READ | PROT_EXEC) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    memcpy(original, orig, sizeof(original));
    if (install_jump(target, (uint64_t)page, original) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    LOGI("hook installed: target=0x%llx tramp=%p detour=%p",
         (unsigned long long)target, page, detour);
    return HOOK_OK;
}

int hook_arm64_guard(uint64_t target, void* check, void* deny) {
    const uint8_t* original = (const uint8_t*)target;
    uint8_t saved[16];
    for (int n = 0; n < 4; ++n) {
        uint32_t word;
        memcpy(&word, original + n * 4, 4);
        if ((word & 0x9F000000u) == 0x90000000u || (word & 0x9F000000u) == 0x10000000u ||
            (word & 0x7C000000u) == 0x14000000u || (word & 0xFF000010u) == 0x54000000u ||
            (word & 0x7E000000u) == 0x34000000u || (word & 0x7E000000u) == 0x36000000u ||
            (word & 0x3B000000u) == 0x18000000u) {
            LOGE("guard target has a PC-relative prologue at +%d", n * 4);
            return HOOK_ERR;
        }
    }
    uint8_t* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return HOOK_ERR;
    uint32_t* code = (uint32_t*)page;
    int i = 0;
    code[i++] = stp_pre(29, 30);
    for (int r = 0; r < 18; r += 2) code[i++] = stp_pre(r, r + 1);
    code[i++] = mov_reg(1, 0);
    code[i++] = 0x58000071; // ldr x17, #12
    code[i++] = 0xD63F0220; // blr x17
    code[i++] = 0x14000003; // b past literal
    uint64_t callback = (uint64_t)check;
    memcpy(&code[i], &callback, 8); i += 2;
    int branch = i++;
    // Restore the engine caller's SP/LR before entering any throwing code.
    for (int r = 16; r >= 0; r -= 2) code[i++] = ldp_post(r, r + 1);
    code[i++] = ldp_post(29, 30);
    code[i++] = 0x58000050; // ldr x16, #8
    code[i++] = 0xD61F0200; // br x16 (tail call)
    uint64_t rejection = (uint64_t)deny;
    memcpy(&code[i], &rejection, 8); i += 2;
    code[branch] = 0x34000000u | ((uint32_t)(i - branch) << 5); // cbz w0, allowed
    for (int r = 16; r >= 0; r -= 2) code[i++] = ldp_post(r, r + 1);
    code[i++] = ldp_post(29, 30);
    memcpy(&code[i], original, 16); i += 4;
    code[i++] = 0x58000050;
    code[i++] = 0xD61F0200;
    uint64_t back = target + 16;
    memcpy(&code[i], &back, 8);
    flush_icache((uint64_t)page, 4096);
    if (page_protection((uint64_t)page, 4096, PROT_READ | PROT_EXEC) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    memcpy(saved, original, sizeof(saved));
    if (install_jump(target, (uint64_t)page, saved) != HOOK_OK) {
        munmap(page, 4096); return HOOK_ERR;
    }
    LOGI("exception-safe guard installed: target=0x%llx", (unsigned long long)target);
    return HOOK_OK;
}
