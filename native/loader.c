#include "loader.h"
#include "engine.h"
#include "bytecode_layout.h"
#include "vm_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t value; uint32_t extra; uint32_t tag; } Value;
_Static_assert(sizeof(Value) == 16, "Roblox TValue size");

struct reader {
    const uint8_t* bytes;
    size_t length, at;
    const char* error;
    void* L;
    void** strings;
    void** protos;
    uint8_t* ups;
    uint32_t string_count, proto_count;
    const char* name;
    const uint64_t* capabilities;
    int result;
};

static const uint8_t* take(struct reader* r, size_t count)
{
    if (r->error) return NULL;
    if (count > r->length - r->at) { r->error = "truncated bytecode"; return NULL; }
    const uint8_t* p = r->bytes + r->at;
    r->at += count;
    return p;
}

static uint8_t byte(struct reader* r)
{
    const uint8_t* p = take(r, 1);
    return p ? *p : 0;
}

static uint64_t varint(struct reader* r, int bits)
{
    uint64_t value = 0;
    for (int shift = 0; shift < bits; shift += 7)
    {
        uint8_t b = byte(r);
        if (r->error) return 0;
        if (bits - shift < 7 && (b & 127u) >= (1u << (bits - shift))) break;
        value |= (uint64_t)(b & 127) << shift;
        if (!(b & 128)) return value;
    }
    r->error = "varint overflow";
    return 0;
}

static uint32_t count(struct reader* r, size_t stride)
{
    uint32_t n = (uint32_t)varint(r, 32);
    if (!r->error && n > (r->length - r->at) / stride)
        r->error = "array count exceeds input";
    return r->error ? 0 : n;
}

static uint32_t word(const uint8_t* p)
{
    uint32_t v; memcpy(&v, p, 4); return v;
}

static void putptr(void* p, size_t off, const void* v) { memcpy((uint8_t*)p + off, &v, 8); }
static void putint(void* p, size_t off, uint32_t v) { memcpy((uint8_t*)p + off, &v, 4); }

static uint32_t string_id(struct reader* r)
{
    uint32_t id = (uint32_t)varint(r, 32);
    if (id > r->string_count) r->error = "invalid string reference";
    return id;
}

static void* string_value(struct reader* r, uint32_t id)
{
    return r->L && id && !r->error ? r->strings[id - 1] : NULL;
}

static void* array(struct reader* r, void* proto, size_t ptr_off, size_t size_off, uint32_t n, size_t stride)
{
    if (!r->L || r->error || !n) return NULL;
    uint8_t memcat = *((uint8_t*)proto + VM_GCO_MEMCAT);
    void* p = g_engine.vm_alloc(r->L, (size_t)n * stride, memcat);
    memset(p, 0, (size_t)n * stride);
    putptr(proto, ptr_off, p);
    putint(proto, size_off, n);
    return p;
}

static int instructions(struct reader* r, const uint8_t* code, uint32_t n, uint32_t* dest)
{
#if defined(BASALT_X86_ENGINE)
    const uint8_t* opcode_map = (const uint8_t*)(g_engine.base + 0x00CD3F60);
#elif defined(BASALT_ARM_744) && BASALT_ARM_744
    const uint8_t* opcode_map = (const uint8_t*)(g_engine.base + 0x0084F959);
#else
    const uint8_t* opcode_map = (const uint8_t*)(g_engine.base + 0x0083FB29);
#endif
    for (uint32_t i = 0; i < n;)
    {
        uint32_t v = word(code + i * 4);
        uint8_t encoded = (uint8_t)v;
        uint8_t op = (uint8_t)(encoded * 203u); // inverse of *227 modulo 256
        if (op >= LOP_NEWCLASSMEMBER || opcode_lengths[op] > n - i)
        { r->error = "unsupported or truncated instruction"; return 0; }
        if (dest)
        {
            uint8_t mapped = opcode_map[encoded];
            dest[i] = (v & 0xffffff00u) | mapped;
            if (opcode_lengths[op] == 2) dest[i + 1] = word(code + (i + 1) * 4);
        }
        i += opcode_lengths[op];
    }
    return 1;
}

static void parse(struct reader* r)
{
    if (byte(r) != 9 || byte(r) != 3) { r->error = "requires Luau v9/type-v3 bytecode"; return; }
    r->string_count = count(r, 1);
    if (r->L && !r->error)
    {
        r->strings = calloc(r->string_count ? r->string_count : 1, sizeof(void*));
        if (!r->strings) { r->error = "out of host memory"; return; }
    }
    for (uint32_t i = 0; i < r->string_count && !r->error; ++i)
    {
        uint32_t size = count(r, 1);
        const uint8_t* str = take(r, size);
        if (r->L && str) r->strings[i] = g_engine.new_string(r->L, (const char*)str, size);
    }
    for (uint8_t type = byte(r); type && !r->error; type = byte(r))
    {
        if (type > 32) { r->error = "invalid userdata type"; break; }
        string_id(r);
    }
    r->proto_count = count(r, 1);
    if (!r->proto_count || r->proto_count > 32768) { r->error = "invalid prototype count"; return; }
    if (r->L && !r->error)
    {
        r->protos = calloc(r->proto_count, sizeof(void*));
        if (!r->protos) { r->error = "out of host memory"; return; }
    }
    void* source = r->L ? g_engine.new_string(r->L, r->name, strlen(r->name)) : NULL;
    void* env = r->L ? *(void**)((uint8_t*)r->L + 0x80) : NULL;
    uint8_t main_nups = 0;
    uint8_t* ups = calloc(r->proto_count, 1);
    r->ups = ups;
    if (!ups) { r->error = "out of host memory"; return; }
    for (uint32_t fid = 0; fid < r->proto_count && !r->error; ++fid)
    {
        uint8_t stack = byte(r), params = byte(r), nups = byte(r), variadic = byte(r), flags = byte(r);
        if (nups > 200 || params > stack || variadic > 1) { r->error = "invalid prototype header"; break; }
        ups[fid] = nups;
        void* p = r->L ? g_engine.new_proto(r->L) : NULL;
        if (p)
        {
            r->protos[fid] = p;
            ((uint8_t*)p)[6] = stack; ((uint8_t*)p)[5] = params;
            ((uint8_t*)p)[3] = nups; ((uint8_t*)p)[7] = variadic;
            ((uint8_t*)p)[4] = flags & 3u; // No native code or inliner state attached.
            putptr(p, 0x30, source); putptr(p, 0x40, r->capabilities); putint(p, 0x9c, fid);
        }
        uint32_t types = count(r, 1);
        take(r, types); // Type information is optional execution metadata.
        uint32_t ncode = count(r, 4);
        const uint8_t* code = take(r, (size_t)ncode * 4);
        if (!code || !ncode) { r->error = "empty or truncated code"; break; }
        uint32_t* instructions_out = array(r, p, 0x28, 0xa8, ncode, 4);
        if (!instructions(r, code, ncode, instructions_out)) break;
        if (p) putptr(p, 0x78, instructions_out);
        uint32_t nk = count(r, 1);
        Value* constants = array(r, p, 0x20, 0xac, nk, sizeof(Value));
        for (uint32_t k = 0; k < nk && !r->error; ++k)
        {
            uint8_t kind = byte(r);
            Value value = {0};
            switch (kind)
            {
            case 0: break;
            case 1: value.tag = 1; value.value = byte(r) != 0; break;
            case 2: {
                const uint8_t* v = take(r, 8);
                value.tag = 3; if (v) memcpy(&value.value, v, 8); break;
            }
            case 3: {
                uint32_t id = string_id(r);
                if (!id) r->error = "nil string constant";
                value.tag = 6; value.value = (uintptr_t)string_value(r, id); break;
            }
            case 4: take(r, 4); break; // Nil import cache uses the interpreter's normal lookup.
            case 5: case 8: {
                uint32_t keys = count(r, kind == 8 ? 5 : 1);
                void* table = r->L ? g_engine.new_table(r->L, 0, keys) : NULL;
                value.tag = 7; value.value = (uintptr_t)table;
                for (uint32_t j = 0; j < keys && !r->error; ++j)
                {
                    uint32_t key = (uint32_t)varint(r, 32);
                    int32_t index = -1;
                    if (kind == 8) { const uint8_t* v = take(r, 4); if (v) index = (int32_t)word(v); }
                    if (key >= k || (index >= 0 && (uint32_t)index >= k))
                    { r->error = "invalid template table constant"; break; }
                    if (table)
                    {
                        if (!constants[key].tag) { r->error = "nil table key"; break; }
                        Value* slot = g_engine.table_set(r->L, table, &constants[key]);
                        if (index >= 0) *slot = constants[index];
                        else *slot = (Value){.tag=3};
                    }
                }
                break;
            }
            case 6: {
                uint32_t child = (uint32_t)varint(r, 32);
                if (child >= fid) { r->error = "invalid closure reference"; break; }
                if (r->L)
                {
                    void* cl = g_engine.new_lclosure(r->L, ups[child], env, r->protos[child]);
                    // 2.742: closure byte 6 is the upvalue count (set by new_lclosure); do not overwrite it.
                    value = (Value){.value=(uintptr_t)cl, .tag=8};
                }
                break;
            }
            case 7: {
                const uint8_t* v = take(r, 16); value.tag = 5;
                if (v) memcpy(&value, v, 12);
                break;
            }
            case 9: {
                uint8_t negative = byte(r); uint64_t v = varint(r, 64);
                value.value = negative ? (~v + 1) : v; value.tag = 4; break;
            }
            default: r->error = "unsupported constant kind"; break;
            }
            if (constants) constants[k] = value;
        }
        uint32_t np = count(r, 1);
        void** children = array(r, p, 0x48, 0xa0, np, 8);
        for (uint32_t j = 0; j < np && !r->error; ++j)
        {
            uint32_t child = (uint32_t)varint(r, 32);
            if (child >= fid) r->error = "invalid child prototype";
            else if (children) children[j] = r->protos[child];
        }
        uint32_t line = (uint32_t)varint(r, 32), name = string_id(r);
        if (p) { putint(p, 0x88, line); putptr(p, 0x50, string_value(r, name)); }
        if (byte(r))
        {
            uint8_t gap = byte(r);
            if (gap > 31) { r->error = "invalid line gap"; break; }
            uint32_t intervals = ((ncode - 1) >> gap) + 1;
            uint32_t absoff = (ncode + 3u) & ~3u;
            uint8_t* lines = array(r, p, 0x10, 0xa4, absoff + intervals * 4, 1);
            const uint8_t* deltas = take(r, ncode);
            const uint8_t* abs = take(r, (size_t)intervals * 4);
            if (lines && deltas && abs)
            {
                uint8_t last = 0; uint32_t baseline = 0;
                for (uint32_t j = 0; j < ncode; ++j) { last += deltas[j]; lines[j] = last; }
                for (uint32_t j = 0; j < intervals; ++j) { baseline += word(abs + j*4); memcpy(lines+absoff+j*4, &baseline, 4); }
                putptr(p, 0x18, lines + absoff); putint(p, 0x94, gap);
            }
        }
        if (byte(r))
        {
            uint32_t locals = count(r, 4);
            uint8_t* vars = array(r, p, 0x08, 0x98, locals, 24);
            for (uint32_t j = 0; j < locals && !r->error; ++j)
            {
                uint32_t id = string_id(r), start = (uint32_t)varint(r, 32), end = (uint32_t)varint(r, 32);
                uint8_t reg = byte(r);
                if (start > end || end > ncode || reg >= stack) r->error = "invalid local debug information";
                if (vars) { putptr(vars+j*24, 0, string_value(r, id)); putint(vars+j*24, 8, start); putint(vars+j*24, 12, end); vars[j*24+16] = reg; }
            }
            uint32_t nu = count(r, 1);
            if (nu != nups) r->error = "invalid upvalue debug information";
            void** names = array(r, p, 0x58, 0x90, nu, 8);
            for (uint32_t j = 0; j < nu && !r->error; ++j)
            { uint32_t id = string_id(r); if (names) names[j] = string_value(r, id); }
        }
    }
    uint32_t main = (uint32_t)varint(r, 32);
    if (main >= r->proto_count) r->error = "invalid main prototype";
    else main_nups = ups[main];
    free(ups);
    r->ups = NULL;
    if (main_nups) r->error = "main prototype has upvalues";
    if (r->at != r->length && !r->error) r->error = "unexpected trailing data";
    if (r->error || !r->L) return;
    void* closure = g_engine.new_lclosure(r->L, 0, env, r->protos[main]);
    if (*((uint8_t*)r->L + VM_L_GC_FLAGS) & 4)
        g_engine.thread_barrier(r->L, r->L, (uint8_t*)r->L + VM_L_GC_LIST);
    Value* top = *(Value**)((uint8_t*)r->L + VM_L_TOP);
    *top = (Value){.value=(uintptr_t)closure, .tag=8};
    *(Value**)((uint8_t*)r->L + VM_L_TOP) = top + 1;
    r->result = 0;
}

int bytecode_validate(const uint8_t* bytes, size_t length, char* error, size_t capacity)
{
    if (!bytes || !length || length > BYTECODE_MAX_BYTES) { snprintf(error, capacity, "invalid bytecode length"); return 1; }
    struct reader r = {.bytes=bytes, .length=length};
    parse(&r);
    free(r.ups);
    if (r.error) snprintf(error, capacity, "%s at byte %zu", r.error, r.at);
    return r.error != NULL;
}

static void build(void* L, void* data) { (void)L; parse(data); }

int bytecode_load(void* L, const char* name, const uint8_t* bytes, size_t length, const uint64_t* capabilities)
{
    char error[180];
    if (bytecode_validate(bytes, length, error, sizeof(error)))
    { g_engine.lua_pushstring(L, error); return 1; }
#if defined(BASALT_ARM_744) && BASALT_ARM_744
    (void)capabilities;
    /* The 2.743 direct Proto constructor has a different object header and
       offsets. Let the verified 2.744 engine construct its own VM objects. */
    return engine_load_submitted(L, name, (const char*)bytes, length, 0);
#endif
    if (!g_engine.check_stack(L, 1)) { g_engine.lua_pushstring(L, "stack limit"); return 1; }
    uintptr_t global = *(uintptr_t*)((uint8_t*)L + VM_L_GLOBAL);
    size_t* threshold = (size_t*)(global + 0x8);
    size_t saved = *threshold;
    size_t offset = *(uintptr_t*)((uint8_t*)L + VM_L_TOP) - *(uintptr_t*)((uint8_t*)L + VM_L_STACK);
    struct reader r = {.bytes=bytes, .length=length, .L=L, .name=name, .capabilities=capabilities, .result=1};
    *threshold = SIZE_MAX;
    int status = g_engine.luaD_pcall(L, build, &r, offset, 0);
    *threshold = saved;
    free(r.strings); free(r.protos); free(r.ups);
    if (status) return 1; // pcall has already left an error value.
    if (r.error) { g_engine.lua_pushstring(L, r.error); return 1; }
    return r.result;
}
