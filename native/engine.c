#include "engine.h"
#include "build_config.h"
#include "hook.h"

#include <android/log.h>
#include <link.h>
#include <string.h>

struct engine g_engine;

// Unique in libroblox.so 2.743.1136. Branch and ADRP displacement bytes are wildcards.
static const uint8_t resume_signature[] = {
    0xFD, 0x7B, 0xBC, 0xA9, 0xF8, 0x5F, 0x01, 0xA9,
    0xF6, 0x57, 0x02, 0xA9, 0xF4, 0x4F, 0x03, 0xA9,
    0xFD, 0x03, 0x00, 0x91, 0x21, 0x00, 0x80, 0x52,
    0xF3, 0x03, 0x00, 0xAA, 0, 0, 0, 0x97,
    0, 0, 0, 0xB4, 0x08, 0x0C, 0x40, 0x39,
    0, 0, 0, 0x90,
};
static const char resume_mask[] =
    "xxxx" "xxxx" "xxxx" "xxxx" "xxxx" "xxxx" "xxxx"
    "???x" "???x" "xxxx" "???x";

static int matches(const uint8_t* data, const uint8_t* pattern, const char* mask, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (mask[i] == 'x' && data[i] != pattern[i])
            return 0;
    return 1;
}

static int discover_library(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    (void)data;
    if (!strstr(info->dlpi_name, "libroblox.so"))
        return 0;

    g_engine.base = (uintptr_t)info->dlpi_addr;
    for (size_t i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X))
        {
            g_engine.text_start = g_engine.base + ph->p_vaddr;
            g_engine.text_size = (size_t)ph->p_filesz;
            break;
        }
    }
    return 1;
}

static int check_entry(uintptr_t offset, uint32_t expected)
{
    uintptr_t address = g_engine.base + offset;
    if (address < g_engine.text_start || address + sizeof(expected) > g_engine.text_start + g_engine.text_size)
        return 0;
    uint32_t actual;
    memcpy(&actual, (const void*)address, sizeof(actual));
    if (actual != expected)
        LOGE("function check failed at +0x%lx: 0x%08x != 0x%08x", (unsigned long)offset, actual, expected);
    return actual == expected;
}

#if defined(__aarch64__) && BASALT_ARM_744
static _Thread_local const char* submitting_bytecode;
static _Thread_local size_t submitting_size;
static struct inline_hook bytecode_gate;

static int arm_bytecode_gate(void* L, const char* name, const char* bytes, size_t size, int env)
{
    int (*original)(void*, const char*, const char*, size_t, int) =
        (void*)bytecode_gate.trampoline;
    int status = original(L, name, bytes, size, env);
    if (status == 1 && bytes == submitting_bytecode && size == submitting_size &&
        size >= 0x118 && size <= 16u * 1024u * 1024u &&
        (unsigned char)bytes[0] == 9 && (unsigned char)bytes[1] == 3) {
        LOGI("submitted bytecode gate accepted size=%zu", size);
        return 0;
    }
    return status;
}

int engine_install_bytecode_gate(void)
{
    const uint32_t expected[4] = {0xD10203FF,0xA9057BFD,0xA90657F6,0xA9074FF4};
    if (hook_arm64_prepare(&bytecode_gate, g_engine.base + 0x02603C10,
                           arm_bytecode_gate, expected) != HOOK_OK)
        return -1;
    if (hook_arm64_activate(&bytecode_gate) == HOOK_OK) return 0;
    hook_arm64_restore(&bytecode_gate);
    return -1;
}

int engine_load_submitted(void* L, const char* name, const char* bytes, size_t size, int env)
{
    const char* previous = submitting_bytecode;
    size_t old_size = submitting_size;
    submitting_bytecode = bytes;
    submitting_size = size;
    int status = g_engine.luau_load(L, name, bytes, size, env);
    submitting_bytecode = previous;
    submitting_size = old_size;
    return status;
}
#else
int engine_install_bytecode_gate(void) { return 0; }
int engine_load_submitted(void* L, const char* name, const char* bytes, size_t size, int env)
{ return g_engine.luau_load(L, name, bytes, size, env); }
#endif

#if defined(__x86_64__)

/* 2.744.1081 x86_64 engine (build c8f12b4f3aedbb4cd385f1398535b2b3551e34e1). */
static const unsigned char x86_build_id[] = {
    0xc8,0xf1,0x2b,0x4f,0x3a,0xed,0xbb,0x4c,0xd3,0x85,
    0xf1,0x39,0x85,0x35,0xb2,0xb3,0x55,0x1e,0x34,0xe1,
};

static int x86_check(uintptr_t offset, uint64_t w0, uint64_t w1) {
    uintptr_t address = g_engine.base + offset;
    uint64_t words[2];
    memcpy(words, (const void*)address, sizeof(words));
    if (words[0] != w0 || words[1] != w1) {
        LOGE("x86 pin failed at +0x%lx: %016llx %016llx != %016llx %016llx",
             (unsigned long)offset,
             (unsigned long long)words[0], (unsigned long long)words[1],
             (unsigned long long)w0, (unsigned long long)w1);
        return 0;
    }
    return 1;
}

struct x86_note_state {
    const ElfW(Phdr)* ph;
    size_t count;
    uintptr_t base;
};

static int x86_note_scan(struct x86_note_state* state) {
    for (size_t i = 0; i < state->count; ++i) {
        const ElfW(Phdr)* ph = state->ph + i;
        if (ph->p_type != PT_NOTE) continue;
        const unsigned char* p = (const void*)(state->base + ph->p_vaddr);
        size_t offset = 0;
        while (ph->p_memsz - offset >= sizeof(ElfW(Nhdr))) {
            ElfW(Nhdr) note;
            memcpy(&note, p + offset, sizeof(note));
            size_t name_size = ((size_t)note.n_namesz + 3) & ~(size_t)3;
            size_t desc_size = ((size_t)note.n_descsz + 3) & ~(size_t)3;
            size_t remaining = ph->p_memsz - offset - sizeof(note);
            if (name_size > remaining || desc_size > remaining - name_size) break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                note.n_descsz == sizeof(x86_build_id) &&
                !memcmp(p + offset + sizeof(note), "GNU", 4) &&
                !memcmp(p + offset + sizeof(note) + name_size, x86_build_id, sizeof(x86_build_id)))
                return 1;
            offset += sizeof(note) + name_size + desc_size;
        }
    }
    return 0;
}

static int x86_discover_note(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    if (!strstr(info->dlpi_name, "libroblox.so")) return 0;
    struct x86_note_state* state = data;
    state->ph = info->dlpi_phdr;
    state->count = info->dlpi_phnum;
    state->base = (uintptr_t)info->dlpi_addr;
    return 1;
}

int engine_init(void) {
    memset(&g_engine, 0, sizeof(g_engine));
    if (dl_iterate_phdr(discover_library, NULL) != 1 || !g_engine.text_start || !g_engine.text_size) {
        LOGE("libroblox.so executable segment not found");
        return -1;
    }
    struct x86_note_state state = {0};
    state.base = g_engine.base;
    // Reuse the phdr walk for the note scan.
    if (dl_iterate_phdr(x86_discover_note, &state) != 1 || !x86_note_scan(&state)) {
        LOGE("x86 engine build ID mismatch; expected 2.744.1081");
        return -1;
    }

    if (!x86_check(0x026F01A8, 0x56415741E5894855ull, 0xFB8948D689415053ull) || // lua_resume
        !x86_check(0x026EB892, 0x56415741E5894855ull, 0xEC83485354415541ull) || // luau_load
        !x86_check(0x026EC3C5, 0x56415741E5894855ull, 0xEC83485354415541ull) || // luaD_pcall
        !x86_check(0x026EC82D, 0x56415741E5894855ull, 0x5589505354415541ull) || // luaD_callint
        !x86_check(0x02698CED, 0x1E7EF685E5894855ull, 0xC148F18958478B48ull) || // lua_type
        !x86_check(0x026FC86D, 0x56415741E5894855ull, 0x4C10EC8348535441ull) || // lua_checkstack
        !x86_check(0x02698048, 0x41535641E5894855ull, 0xA8078AFB8948F689ull) || // lua_pushvalue
        !x86_check(0x026ECBD8, 0x56415741E5894855ull, 0xF78941D389485053ull))   // lua_tolstring
    {
        LOGE("x86 signature mismatch; hook disabled");
        return -1;
    }

    g_engine.resume_entry = g_engine.base + 0x026F01A8;
    g_engine.luau_load = (void*)(g_engine.base + 0x026EB892);
    g_engine.luaD_pcall = (void*)(g_engine.base + 0x026EC3C5);
    g_engine.luaD_callint = (void*)(g_engine.base + 0x026EC82D);
    g_engine.check_stack = (void*)(g_engine.base + 0x026FC86D);
    g_engine.lua_pushvalue = (void*)(g_engine.base + 0x02698048);
    g_engine.lua_pushcclosurek = (void*)(g_engine.base + 0x026991AB);
    g_engine.new_string = (void*)(g_engine.base + 0x02697ADF);
    LOGI("verified x86 build: engine=%p resume=%p code=%zu",
         (void*)g_engine.base, (void*)g_engine.resume_entry, g_engine.text_size);
    return 0;
}

#else /* __aarch64__ */

#if BASALT_ARM_744
#define ARM_ADDR(old_rva, new_rva) (new_rva)
__attribute__((used, retain)) static const char arm_target_build[] =
    "BASALT_ARM_ENGINE_2.744.1081_cd14337ff95a497763bf0f2b08d2359314bc663b";
#else
#define ARM_ADDR(old_rva, new_rva) (old_rva)
#endif

static int arm_build_id(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    if ((uintptr_t)info->dlpi_addr != g_engine.base || !strstr(info->dlpi_name, "libroblox.so"))
        return 0;
#if BASALT_ARM_744
    static const uint8_t expected[] = {
        0xcd,0x14,0x33,0x7f,0xf9,0x5a,0x49,0x77,0x63,0xbf,
        0x0f,0x2b,0x08,0xd2,0x35,0x93,0x14,0xbc,0x66,0x3b,
    };
#else
    static const uint8_t expected[] = {
        0x4a,0x91,0x6f,0x58,0x38,0x6d,0x0c,0x32,0x9a,0x12,
        0x02,0xbf,0xc4,0xdf,0x0c,0x39,0x2e,0x86,0x94,0x87,
    };
#endif
    int* matched = data;
    for (size_t i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_NOTE) continue;
        const uint8_t* notes = (const void*)(g_engine.base + ph->p_vaddr);
        size_t offset = 0;
        while (ph->p_memsz - offset >= sizeof(ElfW(Nhdr)))
        {
            ElfW(Nhdr) note;
            memcpy(&note, notes + offset, sizeof(note));
            size_t name = ((size_t)note.n_namesz + 3) & ~(size_t)3;
            size_t desc = ((size_t)note.n_descsz + 3) & ~(size_t)3;
            if (name > ph->p_memsz - offset - sizeof(note) ||
                desc > ph->p_memsz - offset - sizeof(note) - name) break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                note.n_descsz == sizeof(expected) &&
                !memcmp(notes + offset + sizeof(note), "GNU", 4) &&
                !memcmp(notes + offset + sizeof(note) + name, expected, sizeof(expected)))
                *matched = 1;
            offset += sizeof(note) + name + desc;
        }
    }
    return 1;
}

int engine_init(void)
{
    memset(&g_engine, 0, sizeof(g_engine));
    if (dl_iterate_phdr(discover_library, NULL) != 1 || !g_engine.text_start || !g_engine.text_size)
    {
        LOGE("libroblox.so executable segment not found");
        return -1;
    }

    int build_matches = 0;
    if (dl_iterate_phdr(arm_build_id, &build_matches) != 1 || !build_matches) {
        LOGE("ARM64 engine build ID mismatch");
        return -1;
    }

#if defined(__x86_64__)
    // All function entries and inline hooks below describe ARM64 instructions.
    // Never activate those patches in a native x86_64 process.
    LOGE("native x86_64 engine port pending: ARM64 function pins cannot be used");
    return -1;
#endif

    const uint8_t* text = (const uint8_t*)g_engine.text_start;
    const size_t length = sizeof(resume_signature);
    uintptr_t found = 0;
    unsigned count = 0;
    for (size_t i = 0; i + length <= g_engine.text_size; i += 4)
    {
        if (matches(text + i, resume_signature, resume_mask, length))
        {
            found = g_engine.text_start + i;
            if (++count > 1)
                break;
        }
    }

    if (count != 1 || found != g_engine.base + ARM_ADDR(0x0423246C, 0x042DAEA4) ||
        !check_entry(ARM_ADDR(0x025A0134,0x02603AE8), 0xD102C3FF) || // luau_load
        !check_entry(ARM_ADDR(0x025A0E04,0x026047B8), 0xA9BA7BFD) || // luaD_pcall
        !check_entry(ARM_ADDR(0x025A0EEC,0x026048C0), 0xF9403279) || // luaD_pcall reads stack at +0x60
        !check_entry(ARM_ADDR(0x025A12B8,0x02604C6C), 0xA9BB7BFD) || // luaD_callint
        !check_entry(ARM_ADDR(0x025A60A8,0x02609A1C), ARM_ADDR(0xF9402C08,0xF9403808)) || // lua_gettop entry
        !check_entry(ARM_ADDR(0x0254FD78,0x025B5488), 0xA9BE7BFD) || // lua_setreadonly
        !check_entry(ARM_ADDR(0x0254F9F0,0x025B5104), 0xA9BC7BFD) || // lua_pushcclosurek
        !check_entry(ARM_ADDR(0x025559BC,0x025BB050), 0xA9BD7BFD) || // tagged userdata
        !check_entry(ARM_ADDR(0x0259D6A8,0x026010CC), 0xA9BE7BFD) || // lua_newthread
        !check_entry(ARM_ADDR(0x0259DDD8,0x02601790), 0xA9BE7BFD) || // effective capabilities
        !check_entry(ARM_ADDR(0x0254FFFC,0x025B56EC), 0xA9BE7BFD) || // lua_pushstring
        !check_entry(ARM_ADDR(0x0254988C,0x025AF130), 0xA9BC7BFD) ||
        !check_entry(ARM_ADDR(0x025A0C2C,0x026045E0), 0xA9BD7BFD) ||
        !check_entry(ARM_ADDR(0x025A10D4,0x02604A98), 0xA9BC7BFD) ||
        !check_entry(ARM_ADDR(0x0254DD1C,0x025B35B0), 0xA9BC7BFD) ||
        !check_entry(ARM_ADDR(0x0254D9C4,0x025B3268), ARM_ADDR(0xA9BC7BFD,0xA9BD7BFD)) ||
        !check_entry(ARM_ADDR(0x025557BC,0x025BAE50), 0xA9BD7BFD) ||
        !check_entry(ARM_ADDR(0x025B059C,0x02613F3C), 0xD10103FF) ||
        !check_entry(ARM_ADDR(0x025B7E78,0x0261B6E4), ARM_ADDR(0x39400828,0x39400028)) ||
        !check_entry(ARM_ADDR(0x0254E394,0x025B3C28), 0xA9BE7BFD) ||
        !check_entry(ARM_ADDR(0x032790C0,0x032E1B54), 0xA9BD7BFD) ||
        !check_entry(ARM_ADDR(0x025578D4,0x025BCF40), ARM_ADDR(0xF9402800,0xF9403C00)) ||
        !check_entry(ARM_ADDR(0x0327891C,0x032E1340), 0xA9BF7BFD) || // lua_error
        !check_entry(ARM_ADDR(0x0327CE88,0x032E5900), 0xA9BD7BFD) || // GC mark object
        !check_entry(ARM_ADDR(0x02557384,0x025BC9F4), 0xA9BE7BFD) || // uncached Instance push
        !check_entry(ARM_ADDR(0x025A9A58,0x0260D3E4), 0xA9BE7BFD) || // security context
        !check_entry(ARM_ADDR(0x0232B38C,0x0238FED0), 0xB9400008) || // identity capability lookup
        !check_entry(ARM_ADDR(0x0255641C,0x025BBAB0), ARM_ADDR(0xA9BB7BFD,0xA9BC7BFD)) ||
        !check_entry(ARM_ADDR(0x025570E4,0x025BC754), 0xD100C3FF) || // cached Instance push
        !check_entry(ARM_ADDR(0x01EAEB04,0x01F092D4), 0xA9BE7BFD) || // shared_ptr destructor
        !check_entry(ARM_ADDR(0x02D7844C,0x02E13DFC), 0x8B21C00C) || // XXH32
        !check_entry(ARM_ADDR(0x02CFFEEC,0x02D9BAB0), 0xA9BC7BFD) || // ZSTD_decompress
        !check_entry(ARM_ADDR(0x025B0558,0x02613EF8), 0xA9BE7BFD) || // Variant -> Lua
        !check_entry(ARM_ADDR(0x025BC71C,0x0261FF70), 0xD10643FF) || // reflected property assignment
        !check_entry(ARM_ADDR(0x04A191B0,0x04AEDC60), 0xD10543FF) || // native prompt
        !check_entry(ARM_ADDR(0x050A4264,0x051AFF00), 0xD10183FF) || // left click
        !check_entry(ARM_ADDR(0x050A442C,0x051B00C8), 0xD10183FF) || // right click
        !check_entry(ARM_ADDR(0x050A4388,0x051B0024), 0xD10143FF) || // click replication
        !check_entry(ARM_ADDR(0x05705654,0x05872418), 0xD10283FF) || // touch dispatch
        !check_entry(ARM_ADDR(0x025AA9FC,0x0260E370), 0xD10183FF) || // Lua -> Variant
        !check_entry(ARM_ADDR(0x02508268,0x0256BB20), 0xD10283FF) || // signal<Player>
#if !BASALT_ARM_744
        !check_entry(0x01EC8EA8, 0xA9BF7BFD) || // Variant int manager
#endif
        !check_entry(ARM_ADDR(0x02B83F38,0x02C129B8), 0xA9BF7BFD) || // Variant int64 manager
        !check_entry(ARM_ADDR(0x01EE3E88,0x01F3F684), 0xA9BF7BFD) || // Variant float manager
        !check_entry(ARM_ADDR(0x01F77350,0x01FD4504), 0xA9BF7BFD)) // Variant double manager
    {
        LOGE("signature/API mismatch; hook disabled (resume matches=%u)", count);
        return -1;
    }

    g_engine.resume_entry = found;
    g_engine.luau_load = (void*)(g_engine.base + ARM_ADDR(0x025A0134,0x02603AE8));
    g_engine.luaD_pcall = (void*)(g_engine.base + ARM_ADDR(0x025A0E04,0x026047B8));
    g_engine.luaD_callint = (void*)(g_engine.base + ARM_ADDR(0x025A12B8,0x02604C6C));
    g_engine.lua_gettop = (void*)(g_engine.base + ARM_ADDR(0x025A60A8,0x02609A1C));
    g_engine.lua_setreadonly = (void*)(g_engine.base + ARM_ADDR(0x0254FD78,0x025B5488));
    g_engine.lua_pushcclosurek = (void*)(g_engine.base + ARM_ADDR(0x0254F9F0,0x025B5104));
    g_engine.new_userdata = (void*)(g_engine.base + ARM_ADDR(0x025559BC,0x025BB050));
    g_engine.lua_newthread = (void*)(g_engine.base + ARM_ADDR(0x0259D6A8,0x026010CC));
    g_engine.effective_capabilities = (void*)(g_engine.base + ARM_ADDR(0x0259DDD8,0x02601790));
    g_engine.lua_pushstring = (void*)(g_engine.base + ARM_ADDR(0x0254FFFC,0x025B56EC));
    g_engine.vm_alloc = (void*)(g_engine.base + ARM_ADDR(0x0254988C,0x025AF130));
    g_engine.new_proto = (void*)(g_engine.base + ARM_ADDR(0x025A0C2C,0x026045E0));
    g_engine.new_lclosure = (void*)(g_engine.base + ARM_ADDR(0x025A10D4,0x02604A98));
    g_engine.new_string = (void*)(g_engine.base + ARM_ADDR(0x0254DD1C,0x025B35B0));
    g_engine.new_table = (void*)(g_engine.base + ARM_ADDR(0x0254D9C4,0x025B3268));
    g_engine.table_set = (void*)(g_engine.base + ARM_ADDR(0x025557BC,0x025BAE50));
    g_engine.check_stack = (void*)(g_engine.base + ARM_ADDR(0x025B059C,0x02613F3C));
    g_engine.thread_barrier = (void*)(g_engine.base + ARM_ADDR(0x025B7E78,0x0261B6E4));
    g_engine.table_get = (void*)(g_engine.base + ARM_ADDR(0x0254E82C,0x025B40B0));
    g_engine.table_barrier = (void*)(g_engine.base + ARM_ADDR(0x025578D4,0x025BCF40));
    g_engine.lua_pushvalue = (void*)(g_engine.base + ARM_ADDR(0x0254E394,0x025B3C28));
    g_engine.lua_pushlstring = (void*)(g_engine.base + ARM_ADDR(0x032790C0,0x032E1B54));
    g_engine.lua_pushboolean = (void*)(g_engine.base + ARM_ADDR(0x025A771C,0x0260B0BC));
    g_engine.lua_error = (void*)(g_engine.base + ARM_ADDR(0x0327891C,0x032E1340));
    g_engine.mark_object = (void*)(g_engine.base + ARM_ADDR(0x0327CE88,0x032E5900));
    g_engine.push_instance_uncached = (void*)(g_engine.base + ARM_ADDR(0x02557384,0x025BC9F4));
    g_engine.security_context = (void*)(g_engine.base + ARM_ADDR(0x025A9A58,0x0260D3E4));
    g_engine.identity_capabilities = (void*)(g_engine.base + ARM_ADDR(0x0232B38C,0x0238FED0));
    g_engine.register_userdata_direct = (void*)(g_engine.base + ARM_ADDR(0x0255641C,0x025BBAB0));
    g_engine.push_instance_cached = (void*)(g_engine.base + ARM_ADDR(0x025570E4,0x025BC754));
    g_engine.release_shared = (void*)(g_engine.base + ARM_ADDR(0x01EAEB04,0x01F092D4));
    g_engine.xxhash32 = (void*)(g_engine.base + ARM_ADDR(0x02D7844C,0x02E13DFC));
    g_engine.zstd_decompress = (void*)(g_engine.base + ARM_ADDR(0x02CFFEEC,0x02D9BAB0));
    g_engine.push_variant = (void*)(g_engine.base + ARM_ADDR(0x025B0558,0x02613EF8));
    g_engine.write_property = (void*)(g_engine.base + ARM_ADDR(0x025BC71C,0x0261FF70));
    g_engine.trigger_prompt = (void*)(g_engine.base + ARM_ADDR(0x04A191B0,0x04AEDC60));
    g_engine.click_detector = (void*)(g_engine.base + ARM_ADDR(0x050A4264,0x051AFF00));
    g_engine.right_click_detector = (void*)(g_engine.base + ARM_ADDR(0x050A442C,0x051B00C8));
    g_engine.signal_player = (void*)(g_engine.base + ARM_ADDR(0x02508268,0x0256BB20));
    g_engine.replicate_click = (void*)(g_engine.base + ARM_ADDR(0x050A4388,0x051B0024));
    g_engine.touch_part = (void*)(g_engine.base + ARM_ADDR(0x05705654,0x05872418));
    g_engine.read_variant = (void*)(g_engine.base + ARM_ADDR(0x025AA9FC,0x0260E370));
    LOGI("verified build: engine=%p resume=%p code=%zu", (void*)g_engine.base, (void*)found, g_engine.text_size);
    return 0;
}

#endif /* __x86_64__ */
