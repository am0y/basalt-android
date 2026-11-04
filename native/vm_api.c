#include "vm_api.h"
#include "engine.h"
#include "vm_layout.h"
#include "runtime_protection.h"
#include "compiler_bridge.h"
#include "loader.h"
#include "linux_bytecode_compat.h"
#include "sha384.h"
#include "bytecode_layout.h"
#include "process_memory.h"
#include "device_http.h"
#include "hook.h"
#include "security_policy.h"
#include "build_config.h"
#include "rnet.h"
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

typedef struct { uint64_t value; uint32_t extra, tag; } Value;
static int inactive_proto(void* L);
static int instance_http_index(void* L);
static int instance_http_namecall(void* L);
static int instance_class_named(uintptr_t instance, const char* expected);
static int (*native_debug_info)(void* L);
static uint8_t* ptr(const void* object, size_t offset) { return (uint8_t*)object + offset; }
static void* ref(const void* object, size_t offset) { return *(void**)ptr(object, offset); }
static void* original_environment(void* L)
{
    void* global = ref(L, VM_L_GLOBAL);
    void* main = global ? ref(global, VM_GLOBAL_MAINTHREAD) : NULL;
    return main ? ref(main, VM_L_ENV) : ref(L, VM_L_ENV);
}
static Value* base(void* L) { return ref(L, VM_L_BASE); }
static Value* top(void* L) { return ref(L, VM_L_TOP); }
static int fail(void* L, const char* message)
{
    g_engine.lua_pushstring(L, message);
    g_engine.lua_error(L);
    return 0;
}
static void push(void* L, Value value)
{
    if (!g_engine.check_stack(L, 1)) { fail(L, "stack limit"); return; }
    if (*ptr(L, VM_L_GC_FLAGS) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
    *top(L) = value;
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) + 1;
}
static Value object(void* value, uint32_t tag) { return (Value){.value=(uintptr_t)value, .tag=tag}; }
static Value number(double value)
{
    Value result = {.tag=3};
    memcpy(&result.value, &value, sizeof(value));
    return result;
}
#if BASALT_DEV_BRIDGE
static int runtime_protection_probe_api(void* L)
{
    push(L, number(runtime_protection_state()));
    g_engine.lua_pushboolean(L, runtime_protection_probe());
    return 2;
}
#endif
static void barrier(void* L, void* parent, Value value)
{
    if (value.tag < 6 || !value.value || !(*ptr(parent, VM_GCO_MARK) & 4) ||
        !(*ptr((void*)value.value, VM_GCO_MARK) & 3)) return;
    uint8_t* global = ref(L, VM_L_GLOBAL);
    if (global[VM_GLOBAL_GC_STATE] >= 1 && global[VM_GLOBAL_GC_STATE] <= 3)
        g_engine.mark_object(global, (void*)value.value);
    else
        *ptr(parent, VM_GCO_MARK) = (*ptr(parent, VM_GCO_MARK) & ~7u) | (global[VM_GLOBAL_WHITE] & 3u);
}
static void put(void* L, void* table, Value key, Value value)
{
    * (Value*)g_engine.table_set(L, table, &key) = value;
    if (value.tag >= 6 && (*ptr(table, VM_GCO_MARK) & 4) &&
        (*ptr((void*)value.value, VM_GCO_MARK) & 3))
        g_engine.table_barrier(L, table, (void*)value.value);
}
static void named(void* L, void* table, const char* name, Value value)
{
    Value key = object(g_engine.new_string(L, name, strlen(name)), 6);
    put(L, table, key, value);
}
static void* registry_map(void* L, const char* name)
{
    void* registry = NULL; /* 743 registry offset pending live hunt */
#if defined(BASALT_ARM_744) && BASALT_ARM_744
    g_engine.lua_pushvalue(L, -10000);
    Value slot = top(L)[-1];
    *(Value**)ptr(L, VM_L_TOP) = top(L) - 1;
    if (slot.tag == 7) registry = (void*)slot.value;
    if (!registry) { fail(L, "VM registry unavailable"); return NULL; }
#endif
    Value key = object(g_engine.new_string(L, name, strlen(name)), 6);
    const Value* existing = g_engine.table_get(registry, &key);
    if (existing->tag == 7) return (void*)existing->value;
    void* table = g_engine.new_table(L, 0, 8);
    push(L, object(table, 7));
    void* mt = g_engine.new_table(L, 0, 1);
    push(L, object(mt, 7));
    named(L, mt, "__mode", object(g_engine.new_string(L, "k", 1), 6));
    *(void**)ptr(table, VM_TABLE_METATABLE) = mt;
    put(L, registry, key, object(table, 7));
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 2;
    return table;
}
static Value arg(void* L, int index)
{
    return g_engine.lua_gettop(L) >= index ? base(L)[index - 1] : (Value){0};
}
static int integer(void* L, int index, int minimum)
{
    Value value = arg(L, index);
    double n = 0;
    memcpy(&n, &value.value, 8);
    if (value.tag != 3 || !isfinite(n) || n < minimum || n > 2147483647 || n != (int)n)
    {
        char message[192];
        snprintf(message, sizeof(message), "expected an integer in range (argument %d, type %u, value %.17g)",
                 index, value.tag, value.tag == 3 ? n : 0.0);
        fail(L, message);
        return 0;
    }
    return (int)n;
}
static void* frame(void* L, int level)
{
    uintptr_t ci = (uintptr_t)ref(L, VM_L_CI), first = (uintptr_t)ref(L, VM_L_BASE_CI);
    if ((uintptr_t)level > (ci - first) / 0x30) { fail(L, "stack level out of range"); return NULL; }
    return (void*)(ci - (size_t)level * 0x30);
}
static void* function_arg(void* L, int index, int allow_c)
{
    Value value = arg(L, index);
    if (value.tag == 3)
    {
        void* ci = frame(L, integer(L, index, 0));
#if BASALT_ARM_744
        value = ((Value*)ref(ci, 0))[-1];
#else
        value = *(Value*)ref(ci, 0x18);
#endif
    }
    if (value.tag != 8) { fail(L, "expected a function or stack level"); return NULL; }
    void* closure = (void*)value.value;
    if (*ptr(closure, 3) && ref(closure, VM_CCLOSURE_FUNCTION) == (void*)inactive_proto)
    {
        void* proxies = registry_map(L, "__basalt_inactive_protos_v1");
        Value proxy_key = object(closure, 8);
        const Value* underlying = g_engine.table_get(proxies, &proxy_key);
        if (underlying->tag == 8) return (void*)underlying->value;
    }
    if (!allow_c && *ptr(closure, 3)) { fail(L, "C closures are not supported"); return NULL; }
    return closure;
}
static Value environment_function(void* L, int optional)
{
    Value value = arg(L, 1);
    if (value.tag == 8) return value;
    int level;
    if (optional && !value.tag) level = 1;
    else level = integer(L, 1, 0);
    void* ci = frame(L, level);
    value = *(Value*)ref(ci,
#if BASALT_ARM_744
        8
#else
        0x18
#endif
    );
    if (value.tag != 8) { fail(L, "stack level has no function environment"); return (Value){0}; }
    return value;
}
static int getfenv(void* L)
{
    Value target = arg(L, 1);
    if (target.tag == 3 && integer(L, 1, 0) == 0)
    {
        void* env = ref(L, VM_L_ENV);
        if (!env || *ptr(env, VM_GCO_TYPE) != 7) return fail(L, "thread environment is unavailable");
        *ptr(env, 7) = 0;
        push(L, object(env, 7));
        return 1;
    }
    Value function = environment_function(L, 1);
    void* closure = (void*)function.value;
    void* env = *ptr(closure, 3) ? ref(L, VM_L_ENV) : ref(closure, 0x10);
    if (!env || *ptr(env, VM_GCO_TYPE) != 7) return fail(L, "function environment is unavailable");
    *ptr(env, 7) = 0;
    push(L, object(env, 7));
    return 1;
}
static int setfenv(void* L)
{
    Value env = arg(L, 2);
    if (env.tag != 7) return fail(L, "expected an environment table");
    Value target = arg(L, 1);
    if (target.tag == 3 && integer(L, 1, 0) == 0)
    {
        if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
        *(void**)ptr(L, VM_L_ENV) = (void*)env.value;
        *ptr((void*)env.value, 3) = 0;
        return 0;
    }
    Value function = environment_function(L, 0);
    void* closure = (void*)function.value;
    if (*ptr(closure, 3)) return fail(L, "setfenv cannot change a C function environment");
    *(void**)ptr(closure, 0x10) = (void*)env.value;
    *ptr((void*)env.value, 3) = 0;
    barrier(L, closure, env);
    push(L, function);
    return 1;
}
static int closure_kind(void* L, int is_c)
{
    Value value = arg(L, 1);
    if (value.tag != 8) return fail(L, "expected a function");
    void* closure = function_arg(L, 1, 1);
    g_engine.lua_pushboolean(L, !!*ptr(closure, 3) == is_c);
    return 1;
}
static int iscclosure(void* L) { return closure_kind(L, 1); }
static int islclosure(void* L) { return closure_kind(L, 0); }
static int closure_upvalue_count(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 8) return fail(L, "expected a function");
    void* closure = function_arg(L, 1, 1);
    unsigned count = *ptr(closure, *ptr(closure, 3) ? VM_CCLOSURE_NUPS : VM_LCLOSURE_NUPS);
    push(L, number(count));
    return 1;
}
static int owned(void* closure)
{
    if (*ptr(closure, 3))
    {
        uintptr_t function = (uintptr_t)ref(closure, VM_CCLOSURE_FUNCTION);
        Dl_info target = {0}, self = {0};
        return dladdr((void*)function, &target) && dladdr((void*)owned, &self) &&
               target.dli_fbase == self.dli_fbase;
    }
    return executor_capability_pointer(ref(ref(closure, 0x18), VM_PROTO_CAPABILITIES));
}
static int isexecutorclosure(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 8) return fail(L, "expected a function");
    void* originals = registry_map(L, "__basalt_hook_originals_v1");
    void* origins = registry_map(L, "__basalt_closure_origin_v1");
    for (unsigned depth = 0; depth < 32; ++depth)
    {
        Value original = *(Value*)g_engine.table_get(originals, &value);
        if (original.tag != 8) original = *(Value*)g_engine.table_get(origins, &value);
        if (original.tag != 8 || original.value == value.value) break;
        value = original;
    }
    g_engine.lua_pushboolean(L, owned((void*)value.value));
    return 1;
}
static int checkcaller(void* L)
{
    void* extra = ref(L, VM_L_EXTRA);
    uintptr_t script = 0;
    if (extra && process_memory_read((uintptr_t)extra + 0x98, &script, sizeof(script)) && script)
    { g_engine.lua_pushboolean(L, 0); return 1; }
    uint8_t* ci = ref(L, VM_L_CI);
    uint8_t* first = ref(L, VM_L_BASE_CI);
    int executor_frame = 0;
    for (ci -= 0x30; ci > first; ci -= 0x30)
    {
        Value value =
#if BASALT_ARM_744
            ((Value*)ref(ci, 0))[-1];
#else
            *(Value*)ref(ci, 0x18);
#endif
        if (value.tag == 8 && !*ptr((void*)value.value, 3))
        {
            if (!owned((void*)value.value))
            { g_engine.lua_pushboolean(L, 0); return 1; }
            executor_frame = 1;
        }
    }
    g_engine.lua_pushboolean(L, executor_frame);
    return 1;
}

struct guarded_method {
    uintptr_t function;
    unsigned upvalues;
    Value captured[8];
    const struct security_rule* rule;
};
static struct guarded_method guarded_methods[SECURITY_RULE_COUNT];
static unsigned guarded_count;
static uintptr_t guarded_entries[SECURITY_RULE_COUNT + 8];
static unsigned guarded_entry_count;
static uintptr_t guarded_namecall;
static void* guarded_globals[64];
static unsigned guarded_global_count;
static void* ready_globals[64];
static unsigned ready_global_count;
static atomic_bool service_protection_enabled = ATOMIC_VAR_INIT(1);

static int service_protection(void* L)
{
    Value enabled = arg(L, 1);
    if (enabled.tag != 0)
    {
        if (enabled.tag != 1) return fail(L, "expected protection enabled boolean");
        atomic_store_explicit(&service_protection_enabled, (uint32_t)enabled.value != 0, memory_order_relaxed);
    }
    g_engine.lua_pushboolean(L, atomic_load_explicit(&service_protection_enabled, memory_order_relaxed));
    return 1;
}

static int security_untrusted(void* L)
{
#if BASALT_DEV_BRIDGE
    static atomic_uint security_reports;
    unsigned report = atomic_fetch_add_explicit(&security_reports, 1, memory_order_relaxed);
#endif
    // Only frames from executor-loaded (untrusted-capability) chunks mark a
    // call as executor-driven. Game scripts, engine-internal calls, and this
    // executor's own trusted machinery all pass; game ThreadExtra::script and
    // the guarded-global registry do not need to participate in the decision.
    for (uint8_t* ci = ref(L, VM_L_CI); ci > (uint8_t*)ref(L, VM_L_BASE_CI); ci -= 0x30)
    {
        Value* value =
#if BASALT_ARM_744
            ((Value*)ref(ci, 0)) - 1;
#else
            ref(ci, 0x18);
#endif
        if (value && value->tag == 8 && !*ptr((void*)value->value, 3) && owned((void*)value->value)) {
            const uint64_t* caps = ref(ref((void*)value->value, 0x18), VM_PROTO_CAPABILITIES);
            int trusted = executor_trusted_capability_pointer(caps);
#if BASALT_DEV_BRIDGE
            if (report < 12) LOGI("742 security frame: closure=%p caps=%p trusted=%d",
                                  (void*)value->value, (void*)caps, trusted);
#endif
            if (!trusted) return 1;
        }
    }
    return 0;
}

static int security_name(Value value, const char* expected)
{
    if (value.tag != 6 || !value.value) return 0;
    size_t length = *(uint32_t*)ptr((void*)value.value, 0x14);
    const char* text = (const char*)ptr((void*)value.value, 0x18);
    const char* nul = memchr(text, 0, length);
    if (nul) length = (size_t)(nul - text);
    if (strlen(expected) != length) return 0;
    for (size_t i = 0; i < length; ++i)
    {
        unsigned a = (unsigned char)text[i], b = (unsigned char)expected[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

static int security_class(Value self, const char* expected)
{
    if (self.tag != 9 || !self.value || *(uint32_t*)ptr((void*)self.value, 4) != 16) return 0;
    void* instance = ref((void*)self.value, 0x10);
    if (!instance) return 0;
    const uint8_t* name = ref(ref(instance, 0x18), 8);
    size_t length = name[0] & 1 ? *(size_t*)(name + 8) : name[0] >> 1;
    const char* text = name[0] & 1 ? *(const char**)(name + 16) : (const char*)name + 1;
    return length == strlen(expected) && !memcmp(text, expected, length);
}

static _Thread_local const struct security_rule* security_rejection;
static _Thread_local int input_method_call;
static int guarded_input_method(void* closure)
{
    if (!closure || !*ptr(closure, 3)) return 0;
    uintptr_t function = (uintptr_t)ref(closure, VM_CCLOSURE_FUNCTION);
    for (unsigned i = 0; i < guarded_count; ++i) {
        const struct guarded_method* method = &guarded_methods[i];
        if (strcmp(method->rule->service, "VirtualInputManager") ||
            (strcmp(method->rule->method, "SendKeyEvent") &&
             strcmp(method->rule->method, "SendMouseButtonEvent") &&
             strcmp(method->rule->method, "SendTouchEvent")) ||
             method->function != function || method->upvalues != *ptr(closure, VM_CCLOSURE_NUPS)) continue;
        int match = 1;
        const Value* captured = (const Value*)ptr(closure, 0x30);
        for (unsigned n = 0; n < method->upvalues; ++n)
            if (captured[n].tag != method->captured[n].tag || captured[n].value != method->captured[n].value)
            { match = 0; break; }
        if (match) return 1;
    }
    return 0;
}
static int security_pre(void* L, void* unused)
{
    (void)unused;
    if (!atomic_load_explicit(&service_protection_enabled, memory_order_relaxed)) return 0;
    void* ci = ref(L, VM_L_CI);
    Value* slot = ci ?
#if BASALT_ARM_744
        ((Value*)ref(ci, 0)) - 1 : NULL;
#else
        ref(ci, 0x18) : NULL;
#endif
    if (!slot || slot->tag != 8) return 0;
    void* closure = (void*)slot->value;
    if (!*ptr(closure, 3)) return 0;
    uintptr_t function = (uintptr_t)ref(closure, VM_CCLOSURE_FUNCTION);
    const struct security_rule* denied = NULL;
    Value method = object(ref(L, VM_L_NAMECALL), 6), self = arg(L, 1);
    if (function == guarded_namecall)
    {
        for (unsigned i = 0; i < SECURITY_RULE_COUNT; ++i)
            if (security_name(method, security_rules[i].method) && security_class(self, security_rules[i].service))
            { denied = &security_rules[i]; break; }
    }
    else
    {
        for (unsigned i = 0; i < guarded_count; ++i)
        {
            const struct guarded_method* candidate = &guarded_methods[i];
            if (function != candidate->function || *ptr(closure, VM_CCLOSURE_NUPS) != candidate->upvalues) continue;
            int match = 1;
            const Value* captured = (const Value*)ptr(closure, 0x30);
            for (unsigned n = 0; n < candidate->upvalues; ++n)
                if (captured[n].tag != candidate->captured[n].tag || captured[n].value != candidate->captured[n].value)
                { match = 0; break; }
            // Roblox 2.742.1059: method closures are no longer unique, so verify
            // the live namecall method name against the rule as well.
            if (match && security_class(self, candidate->rule->service)) { denied = candidate->rule; break; }
        }
    }
    if (!denied || (input_method_call && guarded_input_method(closure)) || !security_untrusted(L)) return 0;
    security_rejection = denied;
    return 1;
}
static int security_deny(void* L)
{
    const struct security_rule* denied = security_rejection;
    security_rejection = NULL;
    if (!denied) return fail(L, "Basalt security: native method is blocked");
    char message[192];
    snprintf(message, sizeof(message), "Basalt security: %s.%s is blocked for executor scripts", denied->service, denied->method);
    return fail(L, message);
}

static int security_hook(void* L, uintptr_t function)
{
    for (unsigned i = 0; i < guarded_entry_count; ++i)
        if (guarded_entries[i] == function) return 1;
    if (guarded_entry_count == sizeof(guarded_entries)/sizeof(guarded_entries[0]) ||
        function < g_engine.text_start || function >= g_engine.text_start + g_engine.text_size)
        return fail(L, "Basalt security: invalid native dispatch entry");
    /*
     * Keep the native dispatch table for the Lua security layer, but do not
     * install the ARM64 trampoline here. On 2.744 the trampoline re-enters
     * Luau while the scheduler is being initialized and exhausts the C stack
     * before the editor can create its UI.
     */
    guarded_entries[guarded_entry_count++] = function;
    return 1;
}

static int security_ready(void* L)
{
    int ready = 0;
    for (unsigned i = 0; i < ready_global_count; ++i) if (ready_globals[i] == ref(L, VM_L_GLOBAL)) ready = 1;
    g_engine.lua_pushboolean(L, ready);
    return 1;
}
static int security_seal(void* L)
{
    for (unsigned i = 0; i < ready_global_count; ++i) if (ready_globals[i] == ref(L, VM_L_GLOBAL)) return 0;
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    LOGI("744 security seal: guarded=%p count=%u global=%p", (void*)guarded_namecall,ready_global_count,ref(L,VM_L_GLOBAL));
#endif
    if (!guarded_namecall || ready_global_count == 64) return fail(L, "Basalt security initialization incomplete");
    ready_globals[ready_global_count++] = ref(L, VM_L_GLOBAL);
    return 0;
}
static int security_namecall(void* L)
{
    Value method = arg(L, 1);
    if (method.tag != 8 || !*ptr((void*)method.value, 3)) return fail(L, "Basalt security requires native Instance dispatch");
    uintptr_t function = (uintptr_t)ref((void*)method.value, VM_CCLOSURE_FUNCTION);
    if (guarded_namecall && guarded_namecall != function) return fail(L, "Basalt security dispatch is immutable");
    if (!security_hook(L, function)) return 0;
    guarded_namecall = function;
    for (unsigned i = 0; i < guarded_global_count; ++i) if (guarded_globals[i] == ref(L, VM_L_GLOBAL)) return 0;
    if (guarded_global_count == 64) return fail(L, "Basalt security VM capacity reached");
    guarded_globals[guarded_global_count++] = ref(L, VM_L_GLOBAL);
    return 0;
}
static int security_register(void* L)
{
    const struct security_rule* rule = NULL;
    for (unsigned i = 0; i < SECURITY_RULE_COUNT; ++i)
        if (security_name(arg(L, 1), security_rules[i].service) && security_name(arg(L, 2), security_rules[i].method))
        { rule = &security_rules[i]; break; }
    if (!rule) return fail(L, "unknown security rule");
    for (unsigned i = 0; i < guarded_count; ++i) if (guarded_methods[i].rule == rule) return 0;
    Value value = arg(L, 3);
    if (value.tag != 8 || !*ptr((void*)value.value, 3)) return fail(L, "security rule requires a native method");
    void* closure = (void*)value.value;
    unsigned count = *ptr(closure, VM_CCLOSURE_NUPS);
    if (count > 8 || guarded_count == SECURITY_RULE_COUNT) return fail(L, "unsupported protected method layout");
    struct guarded_method* entry = &guarded_methods[guarded_count];
    entry->function = (uintptr_t)ref(closure, VM_CCLOSURE_FUNCTION);
    entry->upvalues = count;
    memcpy(entry->captured, ptr(closure, 0x30), count * sizeof(Value));
    entry->rule = rule;
    if (!security_hook(L, entry->function)) return 0;
    ++guarded_count;
    return 0;
}
static int security_rule_list(void* L)
{
    void* result = g_engine.new_table(L, SECURITY_RULE_COUNT, 0);
    push(L, object(result, 7));
    for (unsigned i = 0; i < SECURITY_RULE_COUNT; ++i)
    {
        void* row = g_engine.new_table(L, 2, 0);
        push(L, object(row, 7));
        put(L, row, number(1), object(g_engine.new_string(L, security_rules[i].service, strlen(security_rules[i].service)), 6));
        put(L, row, number(2), object(g_engine.new_string(L, security_rules[i].method, strlen(security_rules[i].method)), 6));
        put(L, result, number(i + 1), object(row, 7));
        *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    }
    return 1;
}
static void* current_closure(void* L)
{
#if BASALT_ARM_744
    return (void*)base(L)[-1].value;
#else
    return (void*)((Value*)ref(ref(L, VM_L_CI), 0x18))->value;
#endif
}
static int wrapped_continue(void* L, int status)
{
    if (status)
    {
        Value error = top(L)[-1];
        if (error.tag == 6)
        {
            const char* message = (const char*)ptr((void*)error.value, 0x18);
            size_t length = *(uint32_t*)ptr((void*)error.value, 0x14);
            for (size_t i = 0; i + 3 < length; ++i)
            {
                if (message[i] != ':' || message[i + 1] < '0' || message[i + 1] > '9') continue;
                size_t j = i + 1;
                while (j < length && message[j] >= '0' && message[j] <= '9') ++j;
                if (j + 1 < length && message[j] == ':' && message[j + 1] == ' ')
                { g_engine.lua_pushlstring(L, message + j + 2, length - j - 2); break; }
            }
        }
        g_engine.lua_error(L);
    }
    return g_engine.lua_gettop(L);
}
struct wrapped_call { Value* function; };
static void wrapped_protected(void* L, void* context)
{
    struct wrapped_call* call = context;
    // Native callers (for example table.foreach and error formatters) need a
    // synchronous result. Re-entry is only valid across yieldable VM frames.
    int yieldable =
#if BASALT_ARM_744
        *(uint16_t*)ptr(L, 0x20) <= *(uint16_t*)ptr(L, 0x22);
#else
        *(uint16_t*)ptr(L, 0x80) <= *(uint16_t*)ptr(L, 0x82);
#endif
    g_engine.luaD_callint(L, call->function, -1, yieldable);
}
static int wrapped_invoke(void* L)
{
    void* map = registry_map(L, "__basalt_wrapped_v1");
    Value key = object(current_closure(L), 8);
    Value target = *(Value*)g_engine.table_get(map, &key);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    Value* ci_slot=ref(ref(L,VM_L_CI),0x18);
    Value* frame_base=base(L);
    LOGI("744 wrapped invoke L=%p ci=%p ci_slot=%p cl=%p cltag=%u prev=%p prevtag=%u map=%p target_tag=%u target=%p",
        L,ref(L,VM_L_CI),ci_slot,(void*)key.value,ci_slot ? ci_slot->tag : 0,
        (void*)frame_base[-1].value,frame_base[-1].tag,map,target.tag,(void*)target.value);
#endif
    if (target.tag != 8) return fail(L, "missing wrapped function");
    int count = g_engine.lua_gettop(L);
    if (!g_engine.check_stack(L, 1)) return fail(L, "stack limit");
    if (*ptr(L, VM_L_GC_FLAGS) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
    Value* args = base(L);
    memmove(args + 1, args, count * sizeof(Value));
    args[0] = target;
    *(Value**)ptr(L, VM_L_API_TOP) = args + count + 1;
    void* ci = ref(L, VM_L_CI);
    *(int*)ptr(ci, 0x20) = 0;
    *(unsigned*)ptr(ci, 0x2c) |= 2;
    struct wrapped_call context = {args};
    ptrdiff_t saved = (uint8_t*)args - (uint8_t*)ref(L, 0x60);
    int status = g_engine.luaD_pcall(L, wrapped_protected, &context, saved, 0);
    uint8_t state = *ptr(L, 3);
    if (!status && (state == 1 || state == 6 || state == 0x7f)) return -1;
    ci = ref(L, VM_L_CI);
    *(unsigned*)ptr(ci, 0x2c) &= ~2u;
#if BASALT_ARM_744
    /* 2.744 CallInfo stores its frame limit at +0x18. +0 is the frame
       base; advancing it here corrupts the next interpreter return. */
    if ((uintptr_t)top(L) > (uintptr_t)ref(ci, 0x18)) *(void**)ptr(ci, 0x18) = top(L);
#else
    if ((uintptr_t)top(L) > (uintptr_t)ref(ci, 0)) *(void**)ptr(ci, 0) = top(L);
#endif
    return wrapped_continue(L, status);
}
static int newcclosure(void* L)
{
    Value target = arg(L, 1);
    if (target.tag != 8) return fail(L, "expected a function");
    if (*ptr((void*)target.value, 3)) { push(L, target); return 1; }
    void* map = registry_map(L, "__basalt_wrapped_v1");
    g_engine.lua_pushcclosurek(L, wrapped_invoke, "newcclosure", 0, wrapped_continue);
    put(L, map, top(L)[-1], target);
    put(L, registry_map(L, "__basalt_newcclosure_v1"), top(L)[-1], (Value){.tag=1, .value=1});
    return 1;
}
static int isnewcclosure(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 8) return fail(L, "expected a function");
    Value marker = *(Value*)g_engine.table_get(registry_map(L, "__basalt_newcclosure_v1"), &value);
    g_engine.lua_pushboolean(L, marker.tag == 1 && marker.value);
    return 1;
}
static int unwrap_cclosure(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 8) return fail(L, "expected a function");
    if (*ptr((void*)value.value, 3) && ref((void*)value.value, VM_CCLOSURE_FUNCTION) == (void*)wrapped_invoke)
    {
        void* map = registry_map(L, "__basalt_wrapped_v1");
        Value target = *(Value*)g_engine.table_get(map, &value);
        if (target.tag == 8) { push(L, target); return 1; }
    }
    push(L, value);
    return 1;
}
static void copy_closure_mapping(void* L, void* source, void* destination)
{
    void* markers = registry_map(L, "__basalt_newcclosure_v1");
    Value source_key = object(source, 8);
    Value marker = *(Value*)g_engine.table_get(markers, &source_key);
    put(L, markers, object(destination, 8), marker);
    void* function = ref(source, VM_CCLOSURE_FUNCTION);
    const char* name = function == (void*)wrapped_invoke ? "__basalt_wrapped_v1" :
        (function == (void*)instance_http_index || function == (void*)instance_http_namecall) ? "__basalt_instance_http_v1" :
        function == (void*)inactive_proto ? "__basalt_inactive_protos_v1" : NULL;
    if (!name) return;
    void* map = registry_map(L, name);
    Value key = object(source, 8);
    Value target = *(Value*)g_engine.table_get(map, &key);
    if (target.tag != 8) {
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
        LOGI("744 closure mapping missing source=%p destination=%p fn=%p map=%s value_tag=%u global=%p",
             source, destination, function, name, target.tag, ref(L, VM_L_GLOBAL));
#endif
        fail(L, "native closure mapping is missing");
        return;
    }
    put(L, map, object(destination, 8), target);
}
static void* clone(void* L, void* closure)
{
    unsigned n = *ptr(closure,
#if BASALT_ARM_744
        *ptr(closure,3) ? VM_CCLOSURE_NUPS : VM_LCLOSURE_NUPS
#else
        6
#endif
    );
    void* copy;
    if (*ptr(closure, 3))
    {
        for (unsigned i = 0; i < n; ++i) push(L, ((Value*)ptr(closure, 0x30))[i]);
        g_engine.lua_pushcclosurek(L, ref(closure, VM_CCLOSURE_FUNCTION), "clonefunction", n, ref(closure, 0x20));
        copy = (void*)top(L)[-1].value;
        *(void**)ptr(copy, 0x10) = ref(closure, 0x10);
        *(void**)ptr(copy, VM_CCLOSURE_DEBUGNAME) = ref(closure, VM_CCLOSURE_DEBUGNAME);
        copy_closure_mapping(L, closure, copy);
    }
    else
    {
        copy = g_engine.new_lclosure(L, n, ref(closure, 0x10), ref(closure, 0x18));
        memcpy(ptr(copy, 0x20), ptr(closure, 0x20), n * sizeof(Value));
        // 2.742 closure header: +3 isC, +5 stacksize, +6 nupvalues, +7 preload.
        memcpy(ptr(copy, 4), ptr(closure, 4), 4);
        push(L, object(copy, 8));
    }
    void* originals = registry_map(L, "__basalt_hook_originals_v1");
    Value source = object(closure, 8);
    Value original = *(Value*)g_engine.table_get(originals, &source);
    void* origins = registry_map(L, "__basalt_closure_origin_v1");
    if (original.tag != 8) original = *(Value*)g_engine.table_get(origins, &source);
    if (original.tag == 8) put(L, origins, object(copy, 8), original);
    return copy;
}
static int clonefunction(void* L)
{
    if (arg(L, 1).tag != 8) return fail(L, "expected a function");
    clone(L, (void*)arg(L, 1).value);
    return 1;
}
static int hookfunction(void* L)
{
    Value target = arg(L, 1), replacement = arg(L, 2);
    if (target.tag != 8 || replacement.tag != 8) return fail(L, "expected two functions");
    if (target.value == replacement.value) { clone(L, (void*)target.value); return 1; }
    void* originals = registry_map(L, "__basalt_hook_originals_v1");
    void* closure = (void*)target.value;
    void* original = clone(L, closure);
    if (!((Value*)g_engine.table_get(originals, &target))->tag)
        put(L, originals, target, object(original, 8));
    if (*ptr(closure, 3))
    {
        void* wrapped = registry_map(L, "__basalt_wrapped_v1");
        put(L, wrapped, target, replacement);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
        LOGI("744 hooked closure target=%p map=%p replacement=%p",closure,wrapped,(void*)replacement.value);
#endif
        *(void**)ptr(closure, VM_CCLOSURE_FUNCTION) = wrapped_invoke;
        *(void**)ptr(closure, 0x20) = wrapped_continue;
    }
    else
    {
        void* replacementClosure = (void*)replacement.value;
        if (!*ptr(replacementClosure, 3) && *ptr(replacementClosure, VM_LCLOSURE_NUPS) <= *ptr(closure, VM_LCLOSURE_NUPS))
        {
            unsigned n = *ptr(replacementClosure, VM_LCLOSURE_NUPS);
            *(void**)ptr(closure, 0x18) = ref(replacementClosure, 0x18);
            *(void**)ptr(closure, 0x10) = ref(replacementClosure, 0x10);
            memcpy(ptr(closure, 0x20), ptr(replacementClosure, 0x20), n * sizeof(Value));
            *ptr(closure, VM_LCLOSURE_STACKSIZE) = *ptr(replacementClosure, VM_LCLOSURE_STACKSIZE);
            barrier(L, closure, object(ref(closure, 0x18), 15));
            barrier(L, closure, object(ref(closure, 0x10), 7));
            for (unsigned i = 0; i < n; ++i) barrier(L, closure, ((Value*)ptr(closure, 0x20))[i]);
            push(L, object(original, 8));
            return 1;
        }
        void* env = g_engine.new_table(L, 0, 1);
        push(L, object(env, 7));
        void* mt = g_engine.new_table(L, 0, 1);
        push(L, object(mt, 7));
        named(L, mt, "__index", object(ref(closure, 0x10), 7));
        *(void**)ptr(env, VM_TABLE_METATABLE) = mt;
        named(L, env, "__basalt_hook_dispatch", replacement);
        size_t size = 0;
        const char source[] = "return __basalt_hook_dispatch(...)";
        char* bytes = compile_encoded(source, sizeof(source) - 1, &size);
        if (!bytes) return fail(L, "hook compiler allocation failed");
#if BASALT_ARM_744
        void* oldenv = ref(L, VM_L_ENV);
        *(void**)ptr(L, VM_L_ENV) = env;
#else
        void* oldenv = ref(L, 0x80);
        *(void**)ptr(L, 0x68) = env;
#endif
        const uint64_t* caps = ref(ref(closure, 0x18), VM_PROTO_CAPABILITIES);
        int status = bytecode_load(L, "=hookfunction", (uint8_t*)bytes, size, caps);
#if BASALT_ARM_744
        *(void**)ptr(L, VM_L_ENV) = oldenv;
#else
        *(void**)ptr(L, 0x68) = oldenv;
#endif
        free(bytes);
        if (status) { g_engine.lua_error(L); return 0; }
        void* trampoline = (void*)top(L)[-1].value;
        *(void**)ptr(closure, 0x18) = ref(trampoline, 0x18);
        *(void**)ptr(closure, 0x10) = env;
        *ptr(closure, VM_LCLOSURE_STACKSIZE) = *ptr(trampoline, VM_LCLOSURE_STACKSIZE);
        *ptr(closure, 7) = *ptr(trampoline, 7);
        barrier(L, closure, object(ref(closure, 0x18), 15));
        barrier(L, closure, object(env, 7));
    }
    push(L, object(original, 8));
    return 1;
}
static int restorefunction(void* L)
{
    Value target = arg(L, 1);
    if (target.tag != 8) return fail(L, "expected a function");
    void* originals = registry_map(L, "__basalt_hook_originals_v1");
    Value original = *(Value*)g_engine.table_get(originals, &target);
    if (original.tag != 8) return 0;
    void* closure = (void*)target.value;
    void* backup = (void*)original.value;
    unsigned n = *ptr(backup, *ptr(backup, 3) ? VM_CCLOSURE_NUPS : VM_LCLOSURE_NUPS);
    size_t offset = *ptr(backup, 3) ? 0x30 : 0x20;
    memcpy(ptr(closure, 3), ptr(backup, 3), 4);
    memcpy(ptr(closure, 0x10), ptr(backup, 0x10), offset - 0x10 + n * sizeof(Value));
    barrier(L, closure, object(ref(closure, 0x10), 7));
    if (!*ptr(closure, 3)) barrier(L, closure, object(ref(closure, 0x18), 15));
    for (unsigned i = 0; i < n; ++i) barrier(L, closure, ((Value*)ptr(closure, offset))[i]);
    if (*ptr(closure, 3)) copy_closure_mapping(L, backup, closure);
    put(L, originals, target, (Value){0});
    return 0;
}
static int isfunctionhooked(void* L)
{
    Value target = arg(L, 1);
    if (target.tag != 8) return fail(L, "expected a function");
    void* originals = registry_map(L, "__basalt_hook_originals_v1");
    const Value* original = g_engine.table_get(originals, &target);
    g_engine.lua_pushboolean(L, original->tag == 8);
    return 1;
}
enum { GC_PUBLIC, GC_THREADS, GC_USERDATA };
static int collect_gc_impl(void* L, int kind)
{
    Value option = arg(L, 1);
    if (kind == GC_PUBLIC && option.tag && option.tag != 1) return fail(L, "getgc expects a boolean");
    int tables = option.tag == 1 && (uint32_t)option.value;
    uint8_t* global = *(uint8_t**)((uint8_t*)L + VM_L_GLOBAL);
    unsigned dead = (global[VM_GLOBAL_WHITE] ^ 3u) & 3u;
    size_t count = 0;
    void* result = g_engine.new_table(L, 1024, 0);
    push(L, object(result, 7));
    if (!g_engine.check_stack(L, 2)) return fail(L, "getgc stack limit");
    const char* error = NULL;
    // Discover every GC page: seed from each pointer-looking word in the
    // global_State page tables, then follow prev/next (+0/+8) and
    // gcolistprev/gcolistnext (+0x10/+0x18) links. Pages are deduplicated.
    size_t seen_cap = 4096, seen_count = 0, queue_head = 0;
    uintptr_t* seen = calloc(seen_cap, sizeof(uintptr_t));
    uintptr_t* queue = malloc(seen_cap * sizeof(uintptr_t));
    size_t queue_cap = seen_cap;
    if (!seen || !queue) { free(seen); free(queue); return fail(L, "getgc allocation failed"); }
    #define GC_SEEN_ADD(candidate, added) do { \
        uintptr_t c_ = (candidate); added = 0; \
        if (c_ >= 0x10000 && (c_ & 7) == 0) { \
            size_t h_ = (c_ >> 4) * 11400714819323198485ull % seen_cap; \
            while (seen[h_] && seen[h_] != c_) h_ = (h_ + 1) % seen_cap; \
            if (!seen[h_]) { \
                if ((seen_count + 1) * 2 > seen_cap) { \
                    size_t new_cap = seen_cap * 2; \
                    uintptr_t* grown_ = calloc(new_cap, sizeof(uintptr_t)); \
                    if (grown_) { \
                        for (size_t k_ = 0; k_ < seen_cap; ++k_) if (seen[k_]) { \
                            size_t n_ = (seen[k_] >> 4) * 11400714819323198485ull % new_cap; \
                            while (grown_[n_]) n_ = (n_ + 1) % new_cap; grown_[n_] = seen[k_]; } \
                        free(seen); seen = grown_; seen_cap = new_cap; \
                        h_ = (c_ >> 4) * 11400714819323198485ull % seen_cap; \
                        while (seen[h_]) h_ = (h_ + 1) % seen_cap; \
                    } \
                } \
                seen[h_] = c_; seen_count++; added = 1; \
            } \
        } } while (0)
    for (unsigned root =
#if BASALT_ARM_744
        0x318; root < 0x320
#else
        0x60; root < 0x300
#endif
        && !error; root += 8) {
        int added;
        GC_SEEN_ADD(*(uintptr_t*)(global + root), added);
        if (added) {
            if (queue_head + seen_count > queue_cap) { queue_cap *= 2; queue = realloc(queue, queue_cap * sizeof(uintptr_t)); }
            queue[seen_count - 1] = *(uintptr_t*)(global + root);
        }
    }
    unsigned pages = 0;
    size_t queue_tail = seen_count;
    while (queue_head < queue_tail && !error) {
        uintptr_t page = queue[queue_head++];
        uint8_t header[0x40];
        if (!process_memory_read(page, header, sizeof(header))) continue;
        int32_t size, stride, free_next;
        memcpy(&size, header + 0x20, 4); memcpy(&stride, header + 0x24, 4);
        memcpy(&free_next, header + 0x30, 4);
        if (stride < 16 || size < 0x40 || size > 16 * 1024 * 1024 || stride > size - 0x40 ||
            free_next < -stride || free_next > size - 0x40) continue;
        if (++pages > 262144) { error = "GC page list is unavailable"; break; }
        // 2.744 newpage at 0x32e3aac links allgcopages at +0/+8;
        // allocation removes free-list pages through +0x10/+0x18.
        for (unsigned link = 0; link <
#if BASALT_ARM_744
             2;
#else
             4;
#endif
             ++link) {
            uintptr_t next;
            memcpy(&next, header + link * 8, 8);
            int added;
            GC_SEEN_ADD(next, added);
            if (added) {
                if (queue_tail == queue_cap) { queue_cap *= 2; queue = realloc(queue, queue_cap * sizeof(uintptr_t)); if (!queue) { error = "getgc allocation failed"; break; } }
                queue[queue_tail++] = next;
            }
        }
        if (error) break;
        uint8_t* snapshot = malloc((size_t)size);
        if (!snapshot || !process_memory_read(page, snapshot, (size_t)size)) {
            free(snapshot); continue;
        }
        size_t end = 0x40 + ((size - 0x40) / stride) * stride;
        // GCO free-list links follow the GC header. Unallocated bump slots
        // precede freeNext + stride and must never be interpreted as objects.
        size_t slots = (end - 0x40) / stride;
        uint8_t* is_free = calloc(slots ? slots : 1, 1);
        if (!is_free) { free(snapshot); error = "getgc allocation failed"; break; }
        uintptr_t link = 0;
        memcpy(&link, header + 0x28, 8);
        for (size_t guard = 0; link && guard < slots; ++guard) {
            if (link < page + 0x40 || link >= page + end || (link - page - 0x40) % stride) break;
            size_t slot = (link - page - 0x40) / stride;
            if (is_free[slot]) break;
            is_free[slot] = 1;
            memcpy(&link, snapshot + (link - page) + 8, 8);
        }
        for (size_t at = 0x40 + free_next + stride; at < end; at += stride) {
            if (is_free[(at - 0x40) / stride]) continue;
            unsigned tag = snapshot[at + VM_GCO_TYPE];
            if ((kind == GC_THREADS ? tag != 10 : kind == GC_USERDATA ? tag != 9 :
                (tag != 8 && tag != 9 && !(tables && tag == 7))) ||
                (snapshot[at + VM_GCO_MARK] & 11u) == dead) continue;
            if (count >= (1u << 26)) { error = "getgc result exceeds VM array capacity"; break; }
            Value candidate = (Value){.value=page + at, .tag=tag};
            push(L, candidate);
            Value key = number(++count);
            *(Value*)g_engine.table_set(L, result, &key) = candidate;
            if ((((uint8_t*)result)[VM_GCO_MARK] & 4) &&
                (((uint8_t*)(uintptr_t)candidate.value)[VM_GCO_MARK] & 3))
                g_engine.table_barrier(L, result, (void*)(uintptr_t)candidate.value);
            *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
        }
        free(is_free);
        free(snapshot);
    }
    #undef GC_SEEN_ADD
    free(seen); free(queue);
    if (error) return fail(L, error);
    return 1;
}
static void collect_gc_protected(void* L, void* kind)
{
    collect_gc_impl(L, *(int*)kind);
}
static int collect_gc(void* L, int kind)
{
#if BASALT_ARM_744
    // Result-table growth allocates VM storage. Suppress collection while
    // page snapshots contain raw object pointers, and restore the threshold
    // even when the VM raises an error during allocation or insertion.
    size_t* threshold = (size_t*)ptr(ref(L, VM_L_GLOBAL), VM_GLOBAL_GC_THRESHOLD);
    size_t previous = *threshold;
    *threshold = SIZE_MAX;
    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, VM_L_API_STACK);
    int status = g_engine.luaD_pcall(L, collect_gc_protected, &kind, saved, 0);
    *threshold = previous;
    if (status) { g_engine.lua_error(L); return 0; }
    return 1;
#else
    return collect_gc_impl(L, kind);
#endif
}
static int getgc(void* L) { return collect_gc(L, GC_PUBLIC); }
static int get_threads(void* L) { return collect_gc(L, GC_THREADS); }
static int get_userdata(void* L) { return collect_gc(L, GC_USERDATA); }

static int inactive_proto(void* L) { return fail(L, "inactive prototypes cannot be called"); }
static void push_proto(void* L, void* closure, void* proto)
{
    void* map = registry_map(L, "__basalt_inactive_protos_v1");
    void* underlying = g_engine.new_lclosure(L, *ptr(proto, 3), ref(closure, 0x10), proto);
    push(L, object(underlying, 8));
    g_engine.lua_pushcclosurek(L, inactive_proto, "inactive-proto", 0, NULL);
    put(L, map, top(L)[-1], object(underlying, 8));
}
static int getproto(void* L)
{
    void* closure = function_arg(L, 1, 0);
    void* proto = ref(closure, 0x18);
    int index = integer(L, 2, 1), count = *(int*)ptr(proto, VM_PROTO_CHILD_COUNT);
    if (index > count) return fail(L, "prototype index out of range");
    void* child = ((void**)ref(proto, VM_PROTO_CHILDREN))[index - 1];
    Value active = arg(L, 3);
    if (active.tag && active.tag != 1) return fail(L, "activated must be a boolean");
    if (active.tag == 1 && (uint32_t)active.value)
    {
        // Preserve arguments while getgc reads its optional boolean.
        Value saved = base(L)[0];
        base(L)[0] = (Value){.tag=1};
        getgc(L);
        base(L)[0] = saved;
        void* values = (void*)top(L)[-1].value;
        void* output = g_engine.new_table(L, 0, 4);
        push(L, object(output, 7));
        int length = *(int*)ptr(values, 8), at = 1;
        Value* array = ref(values, 0x18);
        for (int i = 0; i < length; ++i)
        {
            Value value = array[i];
            if (value.tag == 8 && !*ptr((void*)value.value, 3) && ref((void*)value.value, 0x18) == child)
                put(L, output, number(at++), value);
        }
        return 1;
    }
    push_proto(L, closure, child);
    return 1;
}
static int getprotos(void* L)
{
    void* closure = function_arg(L, 1, 0);
    void* proto = ref(closure, 0x18);
    int count = *(int*)ptr(proto, VM_PROTO_CHILD_COUNT);
    void* output = g_engine.new_table(L, count, 0);
    push(L, object(output, 7));
    for (int i = 0; i < count; ++i)
    {
        push_proto(L, closure, ((void**)ref(proto, VM_PROTO_CHILDREN))[i]);
        put(L, output, number(i + 1), top(L)[-1]);
        *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 2;
    }
    return 1;
}
static int getreg(void* L)
{
#if BASALT_ARM_744
    g_engine.lua_pushvalue(L, -10000);
#else
    push(L, *(Value*)ptr(ref(L, 0x50), 0x620));
#endif
    return 1;
}
static int getnamecallmethod(void* L)
{
    void* name = ref(L, VM_L_NAMECALL);
    if (name && *ptr(name, VM_GCO_TYPE) != 6) return fail(L, "unexpected namecall layout");
    push(L, name ? object(name, 6) : (Value){0});
    return 1;
}
static int setnamecallmethod(void* L)
{
    Value name = arg(L, 1);
    if (name.tag != 6) return fail(L, "expected a method name string");
    if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
    *(void**)ptr(L, VM_L_NAMECALL) = (void*)name.value;
    return 0;
}
static int setclipboard(void* L)
{
    Value text = arg(L, 1);
    if (text.tag != 6) return fail(L, "setclipboard expects a string");
    char error[192] = {0};
    size_t length = *(uint32_t*)ptr((void*)text.value, 0x14);
    if (device_setclipboard((const char*)ptr((void*)text.value, 0x18), length, error, sizeof(error)))
        return fail(L, error);
    return 0;
}
static int getclipboard(void* L)
{
    char error[192] = {0};
    char* text = NULL;
    size_t length = 0;
    if (device_getclipboard(&text, &length, error, sizeof(error))) return fail(L, error);
    g_engine.lua_pushlstring(L, text ? text : "", length);
    free(text);
    return 1;
}
static int gethwid(void* L)
{
    char digest[97], error[192];
    if (device_fingerprint(digest, error, sizeof(error))) return fail(L, error);
    g_engine.lua_pushlstring(L, digest, 96);
    return 1;
}
static int setfpscap(void* L)
{
    Value value = arg(L, 1);
    double fps = 0;
    memcpy(&fps, &value.value, sizeof(fps));
    if (value.tag != 3 || !isfinite(fps) || floor(fps) != fps || fps < 0 || fps > 240)
        return fail(L, "setfpscap expects an integer from 0 to 240");
    // The pinned ARM64 scheduler reads TaskSchedulerTargetFps on each worker
    // iteration and clamps it to 240. Zero requests its highest effective cap.
    __atomic_store_n((int32_t*)(g_engine.base + VM_TARGET_FPS), fps == 0 ? 240 : (int32_t)fps, __ATOMIC_RELEASE);
    return 0;
}
static int getfpscap(void* L)
{
    int32_t fps = __atomic_load_n((int32_t*)(g_engine.base + VM_TARGET_FPS), __ATOMIC_ACQUIRE);
    push(L, number(fps > 0 ? fps <= 240 ? fps : 240 : 60));
    return 1;
}
static int device_battery(void* L)
{
    push(L, number(device_battery_percent()));
    return 1;
}
static int input_event(void* L)
{
    int kind = integer(L, 1, 0), code = integer(L, 2, 0);
    if (kind > 11) return fail(L, "unsupported input event");
    float dimensions[4];
    for (int i = 0; i < 4; ++i) {
        Value value = arg(L, i + 3);
        double n;
        memcpy(&n, &value.value, 8);
        if (value.tag != 3 || !isfinite(n) || fabs(n) > 10000000)
            return fail(L, "input coordinates must be finite numbers");
        dimensions[i] = (float)n;
    }
    if (dimensions[2] <= 0 || dimensions[3] <= 0) return fail(L, "game viewport is unavailable");
    char error[192] = {0};
    if (device_input_event(kind, code, dimensions[0], dimensions[1], dimensions[2], dimensions[3], error, sizeof(error)))
        return fail(L, error);
    return 0;
}
struct input_invocation { Value* function; };
static void input_invoke_protected(void* L, void* context)
{
    g_engine.luaD_callint(L, ((struct input_invocation*)context)->function, 0, 0);
}
static int input_native_call(void* L)
{
    Value method = arg(L, 1);
    if (method.tag != 8 || !guarded_input_method((void*)method.value))
        return fail(L, "expected a protected VirtualInputManager input method");
    if (g_engine.lua_gettop(L) < 3) return fail(L, "native input arguments are missing");
    struct input_invocation call = {base(L)};
    ptrdiff_t saved = (uint8_t*)call.function - (uint8_t*)ref(L, 0x60);
    ++input_method_call;
    int status = g_engine.luaD_pcall(L, input_invoke_protected, &call, saved, 0);
    --input_method_call;
    if (status) { g_engine.lua_error(L); return 0; }
    return 0;
}
static int input_foreground(void* L)
{
    g_engine.lua_pushboolean(L, device_input_foreground());
    return 1;
}
static int open_discord_invite(void* L)
{
    char error[192] = {0};
    if (device_open_discord_invite(error, sizeof(error))) return fail(L, error);
    g_engine.lua_pushboolean(L, 1);
    return 1;
}
static int editor_input(void* L)
{
    float bounds[6], state[23] = {0};
    for (int i = 0; i < 6; ++i)
    {
        Value value = arg(L, i + 1);
        double n = 0;
        memcpy(&n, &value.value, sizeof(n));
        if (value.tag != 3 || !isfinite(n) || fabs(n) > 10000000) return fail(L, "invalid editor viewport");
        bounds[i] = (float)n;
    }
    Value enabled = arg(L, 7);
    if (enabled.tag != 1) return fail(L, "expected editor viewport visibility");
    char error[192] = {0};
    if (device_editor_input(bounds, (uint32_t)enabled.value != 0, state, error, sizeof(error)))
        return fail(L, error[0] ? error : "editor input bridge failed");
    for (int i = 0; i < 23; ++i) push(L, number(state[i]));
    return 23;
}
static void* instance_arg(void* L, int index)
{
    Value value = arg(L, index);
    uint8_t instance_tag =
#if BASALT_ARM_744
        0x3b; /* Verified against live 2.744 Instance userdata; engine build-ID pinned. */
#else
        *(uint32_t*)(g_engine.base + 0x06C32098);
#endif
    if (value.tag != 9 || !value.value || *ptr((void*)value.value, 3) != instance_tag ||
        *(uint32_t*)ptr((void*)value.value, 4) != 16)
    { fail(L, "expected an Instance"); return NULL; }
    return ptr((void*)value.value, 0x10);
}
static int cloneref(void* L)
{
    uintptr_t shared[2];
    memcpy(shared, instance_arg(L, 1), sizeof(shared));
    int64_t owners = -1;
    if (!shared[1] || !process_memory_read(shared[1] + 8, &owners, sizeof(owners)) ||
        owners < 0 || owners == INT64_MAX) return fail(L, "Instance lifetime owner is unavailable");
    int tag = *ptr((void*)arg(L, 1).value, 3);
    // The native uncached push still touches engine tracking. Allocate a real
    // tagged wrapper directly and own a strong shared_ptr reference, as on Linux.
    void* payload = g_engine.new_userdata(L, 16, tag);
    memcpy(payload, shared, sizeof(shared));
    __atomic_fetch_add((int64_t*)(shared[1] + 8), 1, __ATOMIC_RELAXED);
    return 1;
}
static int disable_instance_direct_access(void* L)
{
    instance_arg(L, 1);
    void* userdata = (void*)arg(L, 1).value;
    unsigned tag = *ptr(userdata, 3);
    void* global = ref(L, VM_L_GLOBAL);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    LOGI("744 direct registration: global=%p tag=%u userdata+8=%p slot=%p",global,tag,
        ref(userdata,8),ref(global,0x3db0+tag*sizeof(void*)));
#endif
    if (tag >= 128 || !ref(userdata, 8) ||
        ref(global, 0x3db0 + tag * sizeof(void*)) != ref(userdata, 8))
        return fail(L, "unexpected Instance direct-access registration");

    // Existing GET/SET/NAMECALLUDATA instructions bypass the live metatable.
    // Null callbacks make the VM backpatch them to ordinary dispatch, including
    // instructions compiled by game scripts before Basalt attached.
    if (!g_engine.register_userdata_direct(L, tag, NULL, NULL, NULL))
        return fail(L, "Instance direct-access registration unavailable");
    void* direct = ptr(global, 0x920 + tag * 0x48);
    if (ref(direct, 0x30) || ref(direct, 0x38) || ref(direct, 0x40))
        return fail(L, "Instance direct-access callbacks remain active");
    return 0;
}
static int compareinstances(void* L)
{
    void* first = ref(instance_arg(L, 1), 0);
    void* second = ref(instance_arg(L, 2), 0);
    g_engine.lua_pushboolean(L, first == second);
    return 1;
}
static void push_cached_protected(void* L, void* shared)
{
    g_engine.push_instance_cached(L, shared);
}
static int getscriptfromthread(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 10) return fail(L, "expected a thread");
    void* thread = (void*)value.value;
#if BASALT_ARM_744
    // The engine's weak owner survives changes to the script global. Reading
    // the environment instead can return an unrelated Instance or miss a
    // live script whose code deliberately clears that global.
    void* extra = ref(thread, VM_L_EXTRA);
    if (extra)
    {
        struct { void* instance; void* control; } shared = {
            ref(extra, VM_EXTRA_SCRIPT), ref(extra, VM_EXTRA_SCRIPT_CONTROL)};
        if (shared.instance && shared.control)
        {
            int64_t* owners = (int64_t*)ptr(shared.control, 8);
            int64_t count = __atomic_load_n(owners, __ATOMIC_ACQUIRE);
            while (count >= 0 && count < INT64_MAX)
            {
                if (__atomic_compare_exchange_n(owners, &count, count + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, VM_L_API_STACK);
                    int status = g_engine.luaD_pcall(L, push_cached_protected, &shared, saved, 0);
                    g_engine.release_shared(&shared);
                    if (status) { g_engine.lua_error(L); return 0; }
                    return 1;
                }
            }
        }
    }
    push(L, (Value){0});
    return 1;
#else
    void* extra = ref(thread, 0xA0);
    if (extra)
    {
        struct { void* instance; void* control; } shared = {ref(extra, 0x98), ref(extra, 0xa0)};
        if (shared.instance && shared.control)
        {
            int64_t* owners = (int64_t*)ptr(shared.control, 8);
            int64_t count = __atomic_load_n(owners, __ATOMIC_ACQUIRE);
            while (count >= 0 && count < INT64_MAX)
            {
                if (__atomic_compare_exchange_n(owners, &count, count + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
                    int status = g_engine.luaD_pcall(L, push_cached_protected, &shared, saved, 0);
                    g_engine.release_shared(&shared);
                    if (status) { g_engine.lua_error(L); return 0; }
                    return 1;
                }
            }
        }
    }
    Value key = object(g_engine.new_string(L, "script", 6), 6);
    const Value* script = g_engine.table_get(ref(thread, 0x80), &key);
    if (script->tag == 9) { push(L, *script); return 1; }
    uint8_t* first = ref(thread, 0x28);
    for (uint8_t* ci = ref(thread, 0x40); ci > first; ci -= 0x30)
    {
        Value function = *(Value*)ref(ci, 0x18);
        if (function.tag != 8) continue;
        void* environment = ref((void*)function.value, 0x10);
        if (!environment) continue;
        script = g_engine.table_get(environment, &key);
        if (script->tag == 9) { push(L, *script); return 1; }
    }
    push(L, (Value){0});
    return 1;
#endif
}
static int thread_environment(void* L)
{
    Value thread = arg(L, 1);
    if (thread.tag != 10) return fail(L, "expected a thread");
    void* environment = ref((void*)thread.value, VM_L_ENV);
    if (!environment || *ptr(environment, VM_GCO_TYPE) != 7) return fail(L, "thread environment unavailable");
    *ptr(environment, 3) = 0;
    push(L, object(environment, 7));
    return 1;
}
static void* instance_cache(void* L)
{
    void* registry = NULL;
#if BASALT_ARM_744
    g_engine.lua_pushvalue(L, -10000);
    Value slot = top(L)[-1];
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    if (slot.tag == 7) registry = (void*)slot.value;
#endif
    if (!registry) { fail(L, "Instance cache registry unavailable"); return NULL; }
    Value key = {.value=g_engine.base +
#if BASALT_ARM_744
        0x025BC524,
#else
        0x02586450,
#endif
        .tag=2};
    Value cache = *(Value*)g_engine.table_get(registry, &key);
    if (cache.tag != 7) { fail(L, "Instance cache unavailable"); return NULL; }
    return (void*)cache.value;
}
static int cache_operation(void* L, int action)
{
    Value instance_key = {.value=(uintptr_t)ref(instance_arg(L, 1), 0), .tag=2};
    if (action == 2) instance_arg(L, 2);
    void* cache = instance_cache(L);
    if (!action)
    {
        const Value* cached = g_engine.table_get(cache, &instance_key);
        g_engine.lua_pushboolean(L, cached->tag == 9);
        return 1;
    }
    put(L, cache, instance_key, action == 2 ? arg(L, 2) : (Value){0});
    return 0;
}
static int cache_iscached(void* L) { return cache_operation(L, 0); }
static int cache_invalidate(void* L) { return cache_operation(L, 1); }
static int cache_replace(void* L) { return cache_operation(L, 2); }

static int script_data(void* L, char** output, size_t* length, int raw)
{
    *output = NULL;
    *length = 0;
    void* instance = ref(instance_arg(L, 1), 0);
    void* descriptor = ref(instance, 0x18);
    const uint8_t* name = ref(descriptor, 8);
    size_t offset;
    if (name[0] == 24 && !memcmp(name + 1, "ModuleScript", 12)) offset =
#if BASALT_ARM_744
        0x110;
#else
        0x118;
#endif
    else if ((name[0] == 22 && !memcmp(name + 1, "LocalScript", 11)) ||
             (name[0] == 12 && !memcmp(name + 1, "Script", 6))) offset =
#if BASALT_ARM_744
        0x168;
#else
        0x170;
#endif
    else return fail(L, "expected a Script, LocalScript, or ModuleScript");
    const uint8_t* stored = NULL;
    if (!process_memory_read((uintptr_t)instance + offset, &stored, sizeof(stored)))
        return fail(L, "script bytecode storage is unreadable");
    if (!stored) return 0;
    uintptr_t text_address = (uintptr_t)stored +
#if BASALT_ARM_744
        0x10;
#else
        0x18;
#endif
    uint8_t text[24];
    if (!process_memory_read(text_address, text, sizeof(text)))
        return fail(L, "script bytecode string is unreadable");
    size_t size;
    const uint8_t* source;
    if (text[0] & 1) { memcpy(&size, text + 8, 8); memcpy(&source, text + 16, 8); }
    else { size = text[0] >> 1; source = (const uint8_t*)(text_address + 1); }
    if (!size) return 0;
    if (!source || size < 8 || size > BYTECODE_MAX_BYTES) return fail(L, "invalid protected bytecode size");
    uint8_t* decoded = malloc(size);
    if (!decoded) return fail(L, "bytecode allocation failed");
    if (!process_memory_read((uintptr_t)source, decoded, size))
    { free(decoded); return fail(L, "script bytecode bytes are unreadable"); }
    if (raw)
    {
        *output = (char*)decoded; *length = size;
        return 1;
    }
    // Packaged CorePackages have an encoded sentinel header followed by plain
    // bytecode. The protected-container format is shared by both Android ABIs.
    static const uint8_t plain_header[8] = {0x52, 0x7a, 0x10, 0x4a, 0x5b, 0x32, 0x09, 0xe0};
    if (size > 10 && !memcmp(decoded, plain_header, sizeof(plain_header)))
    {
        if (decoded[8] < 3 || decoded[8] > 13)
        { free(decoded); return fail(L, "unsupported packaged bytecode version"); }
        memmove(decoded, decoded + 8, size - 8);
        *output = (char*)decoded; *length = size - 8;
        return 1;
    }
    uint8_t key[4];
    for (unsigned i = 0; i < 4; ++i) key[i] = (uint8_t)((decoded[i] ^ "RSB1"[i]) - i * 41u);
    for (size_t i = 0; i < size; ++i) decoded[i] ^= (uint8_t)(key[i % 4] + i * 41u);
    uint32_t expected_hash, expected_size;
    memcpy(&expected_hash, key, 4);
    memcpy(&expected_size, decoded + 4, 4);
    if (memcmp(decoded, "RSB1", 4) || g_engine.xxhash32(decoded, (int)size, 42) != expected_hash ||
        expected_size > BYTECODE_MAX_BYTES)
    { free(decoded); return fail(L, "protected bytecode validation failed"); }
    if (!expected_size)
    {
        memmove(decoded, decoded + 8, size - 8);
        *output = (char*)decoded; *length = size - 8;
        return 1;
    }
    char* bytes = malloc(expected_size);
    if (!bytes) { free(decoded); return fail(L, "bytecode allocation failed"); }
    size_t actual = g_engine.zstd_decompress(bytes, expected_size, decoded + 8, size - 8);
    free(decoded);
    if (actual != expected_size) { free(bytes); return fail(L, "script bytecode decompression failed"); }
    *output = bytes; *length = expected_size;
    return 1;
}
static int compatible_script_data(void* L, char** bytes, size_t* length, int* converted)
{
    *converted = 0;
    if (!script_data(L, bytes, length, 0)) return 0;
    // Keep Android's existing larger-chunk support; the bounded converter
    // handles at most 4 MiB and leaves unsupported newer formats untouched.
    if (*length > 4u * 1024u * 1024u) return 1;
    char* lowered = NULL;
    size_t size = 0;
    char error[192];
    int status = linux_bytecode_compat(*bytes, *length, &lowered, &size, error, sizeof(error));
    if (status < 0)
    {
        free(*bytes); *bytes = NULL; *length = 0;
        return fail(L, error);
    }
    if (status > 0)
    {
        free(*bytes); *bytes = lowered; *length = size; *converted = 1;
    }
    return 1;
}
static int script_bytecode(void* L, int compatible)
{
    char* bytes; size_t length;
    int converted;
    int found = compatible ? compatible_script_data(L, &bytes, &length, &converted) :
        script_data(L, &bytes, &length, 0);
    if (!found) { push(L, (Value){0}); return 1; }
    g_engine.lua_pushlstring(L, bytes, length);
    free(bytes);
    return 1;
}
static int getscriptbytecode(void* L) { return script_bytecode(L, 1); }
static int get_raw_script_bytecode(void* L) { return script_bytecode(L, 0); }
static int getscriptclosure(void* L)
{
    char* bytes; size_t length;
    int converted;
    if (!compatible_script_data(L, &bytes, &length, &converted)) { push(L, (Value){0}); return 1; }
    // Rebuilt chunks have no engine authentication trailer. Load them with our
    // validated v9 loader, then assign persistent capabilities before returning.
    int status = converted ? bytecode_load(L, "=getscriptclosure", (const uint8_t*)bytes, length, NULL) :
        g_engine.luau_load(L, "=getscriptclosure", bytes, length, 0);
    free(bytes);
    if (status) { g_engine.lua_error(L); return 0; }
    if (!executor_prepare_closure(L)) return fail(L, "script closure capabilities unavailable");
    return 1;
}
static int getscripthash(void* L)
{
    char* bytes; size_t length;
    if (!script_data(L, &bytes, &length, 1)) { push(L, (Value){0}); return 1; }
    Sha384 hash;
    char digest[97];
    sha384_init(&hash); sha384_update(&hash, bytes, length); sha384_finish(&hash, digest);
    free(bytes);
    g_engine.lua_pushlstring(L, digest, 96);
    return 1;
}
static int module_result_loaded(void* L, uintptr_t entry)
{
    uint64_t words[6] = {0};
    if (!process_memory_read(entry, words, sizeof(words))) return 0;
    int32_t state = (int32_t)words[1], error_reference = (int32_t)(words[1] >> 32);
    int32_t result_reference = (int32_t)words[4];
    uintptr_t global = 0;
    if (state != 3 || error_reference != -1 || result_reference < 0 || !words[3] ||
        !process_memory_read(words[3] + VM_MODULE_THREAD_GLOBAL, &global, sizeof(global)) || global != (uintptr_t)ref(L, VM_L_GLOBAL))
        return 0;
    Value key = number(result_reference);
    getreg(L);
    Value registry = top(L)[-1];
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    return registry.tag == 7 && ((Value*)g_engine.table_get((void*)registry.value, &key))->tag != 0;
}
static int module_loaded(void* L)
{
    void* instance = ref(instance_arg(L, 1), 0);
    const uint8_t* name = ref(ref(instance, 0x18), 8);
    if (name[0] != 24 || memcmp(name + 1, "ModuleScript", 12)) return fail(L, "expected a ModuleScript");
    int loaded = module_result_loaded(L, (uintptr_t)instance + VM_MODULE_RESULT);
    uintptr_t cache = 0, node = 0;
    if (!loaded && process_memory_read((uintptr_t)instance + VM_MODULE_CACHE, &cache, 8) && cache &&
        process_memory_read(cache + 0x10, &node, 8))
    {
        for (unsigned count = 0; node && count < 4096; ++count)
        {
            if (module_result_loaded(L, node + 0x10)) { loaded = 1; break; }
            uintptr_t next = 0;
            if (!process_memory_read(node, &next, 8) || next == node) break;
            node = next;
        }
    }
    g_engine.lua_pushboolean(L, loaded);
    return 1;
}
static int instance_http_dispatch(void* L, int namecall)
{
    void* saved_namecall = ref(L, 0x60);
    void* map = registry_map(L, "__basalt_instance_http_v1");
    Value current = object(current_closure(L), 8);
    Value original = *(Value*)g_engine.table_get(map, &current);
    if (original.tag != 8) return fail(L, "missing original Instance dispatch");
    Value config_key = object(g_engine.new_string(L, "config", 6), 6);
    Value config = *(Value*)g_engine.table_get(map, &config_key);
    if (config.tag == 7)
    {
        Value game_key = number(1), method_key = number(2);
        Value data_model = *(Value*)g_engine.table_get((void*)config.value, &game_key);
        Value self = arg(L, 1);
        if (self.tag == 9 && data_model.tag == 9 && ref((void*)self.value, 0x10) == ref((void*)data_model.value, 0x10))
        {
            Value method = namecall ? object(saved_namecall, 6) : arg(L, 2);
            if (method.tag == 6 && method.value)
            {
                uint32_t length = *(uint32_t*)ptr((void*)method.value, 0x14);
                const char* name = (const char*)ptr((void*)method.value, 0x18);
                if ((length == 7 && !memcmp(name, "HttpGet", 7)) || (length == 12 && !memcmp(name, "HttpGetAsync", 12)))
                {
                    Value callback = *(Value*)g_engine.table_get((void*)config.value, &method_key);
                    if (!namecall) { push(L, callback); return 1; }
                    int count = g_engine.lua_gettop(L);
                    if (!g_engine.check_stack(L, 1)) return fail(L, "stack limit");
                    if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
                    Value* args = base(L);
                    memmove(args + 1, args, count * sizeof(Value));
                    args[0] = callback;
                    *(Value**)ptr(L, VM_L_API_TOP) = args + count + 1;
                    void* ci = ref(L, 0x40);
                    *(int*)ptr(ci, 0x20) = 0;
                    *(unsigned*)ptr(ci, 0x2c) |= 2;
                    struct wrapped_call context = {args};
                    ptrdiff_t saved = (uint8_t*)args - (uint8_t*)ref(L, 0x60);
                    int status = g_engine.luaD_pcall(L, wrapped_protected, &context, saved, 0);
                    uint8_t state = *ptr(L, 3);
                    if (!status && (state == 1 || state == 6 || state == 0x7f)) return -1;
                    ci = ref(L, 0x40);
                    *(unsigned*)ptr(ci, 0x2c) &= ~2u;
                    if ((uintptr_t)top(L) > (uintptr_t)ref(ci, 0)) *(void**)ptr(ci, 0) = top(L);
                    return wrapped_continue(L, status);
                }
            }
        }
    }
    *(void**)ptr(L, 0x60) = saved_namecall;
    if (!namecall)
    {
        int (*callback)(void*) = ref((void*)original.value, 0x18);
        return callback(L);
    }
    // Keep the engine method in its own native frame. A direct C call would
    // leave its asynchronous resumer attached to our unrelated continuation.
    int count = g_engine.lua_gettop(L);
    if (!g_engine.check_stack(L, 1)) return fail(L, "stack limit");
    if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
    Value* args = base(L);
    memmove(args + 1, args, count * sizeof(Value));
    args[0] = original;
    *(Value**)ptr(L, VM_L_API_TOP) = args + count + 1;
    void* ci = ref(L, 0x40);
    *(int*)ptr(ci, 0x20) = 0;
    *(unsigned*)ptr(ci, 0x2c) |= 2;
    struct wrapped_call context = {args};
    ptrdiff_t saved = (uint8_t*)args - (uint8_t*)ref(L, 0x60);
    int status = g_engine.luaD_pcall(L, wrapped_protected, &context, saved, 0);
    uint8_t state = *ptr(L, 3);
    if (!status && (state == 1 || state == 6 || state == 0x7f)) return -1;
    ci = ref(L, 0x40);
    *(unsigned*)ptr(ci, 0x2c) &= ~2u;
    if ((uintptr_t)top(L) > (uintptr_t)ref(ci, 0)) *(void**)ptr(ci, 0) = top(L);
    return wrapped_continue(L, status);
}
static int instance_http_index(void* L) { return instance_http_dispatch(L, 0); }
static int instance_http_namecall(void* L) { return instance_http_dispatch(L, 1); }
static int install_instance_http(void* L)
{
    Value data_model = arg(L, 1), callback = arg(L, 2);
    instance_arg(L, 1);
    if (callback.tag != 8) return fail(L, "expected an HTTP method callback");
    void* mt = ref((void*)data_model.value, 8);
    void* map = registry_map(L, "__basalt_instance_http_v1");
    Value config_key = object(g_engine.new_string(L, "config", 6), 6);
    Value installed = *(Value*)g_engine.table_get(map, &config_key);
    if (installed.tag == 7)
    {
        Value game_key = number(1);
        Value previous = *(Value*)g_engine.table_get((void*)installed.value, &game_key);
        if (previous.tag == 9 && ref((void*)previous.value, 0x10) == ref((void*)data_model.value, 0x10))
            return 0; // Preserve metamethods legitimately hooked by scripts since installation.
    }
    void* config = g_engine.new_table(L, 2, 0);
    push(L, object(config, 7));
    put(L, config, number(1), data_model);
    put(L, config, number(2), callback);
    const char* names[] = {"__index", "__namecall"};
    int (*functions[])(void*) = {instance_http_index, instance_http_namecall};
    for (int i = 0; i < 2; ++i)
    {
        Value key = object(g_engine.new_string(L, names[i], strlen(names[i])), 6);
        Value target = *(Value*)g_engine.table_get(mt, &key);
        if (target.tag != 8 || !*ptr((void*)target.value, 3)) return fail(L, "Instance dispatch is not native");
        void* closure = (void*)target.value;
        if (ref(closure, 0x18) == (void*)functions[i]) continue;
        if (ref(closure, 0x20)) return fail(L, "unexpected Instance dispatch continuation");
        clone(L, closure);
        put(L, map, target, top(L)[-1]);
        *(void**)ptr(closure, 0x18) = functions[i];
        if (i == 1) *(void**)ptr(closure, 0x20) = wrapped_continue;
    }
    named(L, map, "config", object(config, 7));
    return 0;
}
static int getthreadidentity(void* L)
{
    void* extra = ref(L, VM_L_EXTRA);
    if (!extra) return fail(L, "thread has no engine context");
    push(L, number(*(int32_t*)ptr(extra, VM_EXTRA_IDENTITY)));
    return 1;
}
void vm_sync_thread_context(void* L)
{
    void* extra = ref(L, VM_L_EXTRA);
    if (!extra) return;
    void* context = g_engine.security_context(L);
    memcpy(context, ptr(extra, VM_EXTRA_IDENTITY), 0x20);
    *(uint64_t*)ptr(context, 0x30) = *(uint64_t*)ptr(extra, VM_EXTRA_CAPABILITIES);
    *(void**)ptr(context, 0x38) = NULL;
}
int vm_copy_thread_capabilities(void* destination, void* source)
{
#if defined(BASALT_ARM_744) && BASALT_ARM_744
    // This hook prepares a newly created executor root. Roblox's userthread
    // callback inherits the current donor's weak script owner even when the
    // root was allocated from the VM main thread. Executor coroutines must
    // inherit an empty owner instead of that unrelated game script.
    void* extra = ref(destination, VM_L_EXTRA);
    if (!extra) return 0;
    void* control = ref(extra, VM_EXTRA_SCRIPT_CONTROL);
    *(void**)ptr(extra, VM_EXTRA_SCRIPT) = NULL;
    *(void**)ptr(extra, VM_EXTRA_SCRIPT_CONTROL) = NULL;
    if (control) ((void(*)(void*))(g_engine.base + 0x02b534e8))(control);
    (void)source;
    return 1;
#else
    void* extra = ref(destination, VM_L_EXTRA);
    if (!extra || !ref(source, VM_L_EXTRA)) return 1; /* 743: no direct ctx */
    // Match Roblox's userthread callback at +0x2634fa4..+0x2634fac:
    // child.extra.capabilities = effective_capabilities(parent).
    // The VM main thread supplies lifetime ownership, but has a zero mask.
    *(uint64_t*)ptr(extra, VM_EXTRA_CAPABILITIES) = g_engine.effective_capabilities(source);
    return 1;
#endif
}
int vm_set_thread_identity(void* L, uint32_t identity)
{
    void* extra = ref(L, VM_L_EXTRA);
    if (!extra) return 0;
    *(uint32_t*)ptr(extra, VM_EXTRA_IDENTITY) = identity;
    uint64_t low = g_engine.identity_capabilities(ptr(extra, VM_EXTRA_IDENTITY));
    uint64_t* caps = (uint64_t*)ptr(extra, VM_EXTRA_CAPABILITIES);
    *caps = (*caps & ~UINT64_C(255)) | low;
    vm_sync_thread_context(L);
    return 1;
}
static int setthreadidentity(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 3) return fail(L, "setthreadidentity expects a number");
    double requested;
    memcpy(&requested, &value.value, sizeof(requested));
    // Match ARM64's numeric-to-integer conversion used by the engine API.
    int32_t identity = isnan(requested) ? 0 : requested >= INT32_MAX ? INT32_MAX :
        requested <= INT32_MIN ? INT32_MIN : (int32_t)requested;
    if (!vm_set_thread_identity(L, (uint32_t)identity))
        return fail(L, "thread has no engine context");
    return 0;
}
#ifdef BASALT_ENABLE_SECURITY_PROBE
static int inspect_instance_layout(void* L)
{
    Value value = arg(L, 1);
    void* shared = instance_arg(L, 1);
    void* instance = ref(shared, 0);
    void* mt = ref((void*)value.value, 8);
    void* result = g_engine.new_table(L, 0, 12);
    push(L, object(result, 7));
    for (unsigned i = 0; i < 24; ++i)
    {
        uintptr_t address = (uintptr_t)((void**)ref(instance, 0))[i];
        if (address >= g_engine.text_start && address < g_engine.text_start + g_engine.text_size)
            put(L, result, number(i * 8), number(address - g_engine.base));
    }
    const char* names[] = {"__index", "__newindex", "__namecall"};
    for (unsigned i = 0; i < 3; ++i)
    {
        Value key = object(g_engine.new_string(L, names[i], strlen(names[i])), 6);
        Value function = *(Value*)g_engine.table_get(mt, &key);
        if (function.tag == 8 && *ptr((void*)function.value, 3))
            named(L, result, names[i], number((uintptr_t)ref((void*)function.value, 0x18) - g_engine.base));
    }
    return 1;
}
static int read_memory(int fd, uintptr_t address, void* output, size_t size);

static int probe_object_layout(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 9 && value.tag != 8 && value.tag != 7 && value.tag != 10)
        return fail(L, "expected userdata, closure, table, or thread");
    void* object_ptr = (void*)value.value;
    if (!object_ptr) return fail(L, "expected a non-null object");
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    void* output = g_engine.new_table(L, 0, 16);
    push(L, object(output, 7));
    named(L, output, "type", number(value.tag));
    named(L, output, "address", number((uintptr_t)object_ptr));
    named(L, output, "base", number((double)g_engine.base));
    if (value.tag == 9)
    {
        uint8_t header[24];
        if (!read_memory(fd, (uintptr_t)object_ptr, header, sizeof(header)))
        { close(fd); return fail(L, "unreadable userdata header"); }
        named(L, output, "tag", number(header[3]));
        unsigned length;
        memcpy(&length, header + 4, 4);
        named(L, output, "size", number(length));
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        uintptr_t word = 0;
        if (read_memory(fd, (uintptr_t)object_ptr + i * 8, &word, sizeof(word)))
        {
            char key[16], text[24];
            snprintf(key, sizeof(key), "w%u", i);
            snprintf(text, sizeof(text), "0x%016llx", (unsigned long long)word);
            named(L, output, key, object(g_engine.new_string(L, text, strlen(text)), 6));
        }
    }
    close(fd);
    return 1;
}
static int probe_global_hex(void* L)
{
    uint8_t* global = *(uint8_t**)((uint8_t*)L + VM_L_GLOBAL);
    int offset = integer(L, 1, 0), length = integer(L, 2, 64);
    if (offset < 0 || length < 1 || length > 512) return fail(L, "bad range");
    uint8_t bytes[512];
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    int readable = read_memory(fd, (uintptr_t)(global + offset), bytes, (size_t)length);
    if (fd >= 0) close(fd);
    if (!readable) return fail(L, "global not readable");
    char hex[1100];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < length; ++i) { hex[i*2] = digits[bytes[i] >> 4]; hex[i*2+1] = digits[bytes[i] & 15]; }
    g_engine.lua_pushlstring(L, hex, (size_t)length * 2);
    return 1;
}
static int probe_memory_hex(void* L)
{
    Value address = arg(L, 1);
    if (address.tag != 6) return fail(L, "expected a hexadecimal address");
    uint32_t chars = *(uint32_t*)ptr((void*)address.value, 0x14);
    if (!chars || chars >= 32) return fail(L, "invalid hexadecimal address");
    char text[32] = {0};
    memcpy(text, ptr((void*)address.value, 0x18), chars);
    char* end = NULL;
    errno = 0;
    uintptr_t pointer = strtoull(text, &end, 0);
    if (errno || !pointer || end != text + chars) return fail(L, "invalid hexadecimal address");
    int length = integer(L, 2, 1);
    if (length > 512) return fail(L, "inspection limit is 512 bytes");
    uint8_t bytes[512];
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    int readable = read_memory(fd, pointer, bytes, (size_t)length);
    if (fd >= 0) close(fd);
    if (!readable) return fail(L, "address is not readable");
    char hex[1024];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < length; ++i)
    {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 15];
    }
    g_engine.lua_pushlstring(L, hex, (size_t)length * 2);
    return 1;
}
#endif
static void* connection_arg(void* L, int index)
{
    Value value = arg(L, index);
    uint8_t tag = 8; // 2.742 RBXScriptConnection userdata tag (probe-verified)
    if (value.tag != 9 || *ptr((void*)value.value, 3) != tag ||
        *(uint32_t*)ptr((void*)value.value, 4) != 24)
    { fail(L, "expected an RBXScriptConnection"); return NULL; }
    return ptr((void*)value.value, 0x10);
}
static int read_memory(int fd, uintptr_t address, void* output, size_t size)
{
    return address && ((fd >= 0 && pread(fd, output, size, (off_t)address) == (ssize_t)size) ||
        process_memory_read(address, output, size));
}
struct reflected_property {
    uintptr_t instance, descriptor;
    uint32_t type;
    _Alignas(8) uint8_t fields[0xb0];
};
static int reflection_lookup(uintptr_t instance, const char* name, size_t length, struct reflected_property* out)
{
    uintptr_t klass = 0, properties[2] = {0}, entries[512][2];
    if (length > 255 || memchr(name, 0, length)) return 0;
    if (!process_memory_read(instance + 0x18, &klass, sizeof(klass)) ||
        !process_memory_read(klass + 0x40, properties, sizeof(properties)) ||
        properties[1] > 512 ||
        (properties[1] && !process_memory_read(properties[0], entries, properties[1] * sizeof(entries[0])))) return -1;
    for (size_t i = 0; i < properties[1]; ++i)
    {
        uintptr_t name_pointer = 0;
        uint64_t text[3];
        char bytes[256];
        if (!process_memory_read(entries[i][0] + 8, &name_pointer, sizeof(name_pointer)) ||
            !process_memory_read(name_pointer, text, sizeof(text))) return -1;
        size_t size = (text[0] & 1) ? text[1] : *(uint8_t*)text >> 1;
        if (size != length) continue;
        uintptr_t address = (text[0] & 1) ? text[2] : name_pointer + 1;
        if (length && !process_memory_read(address, bytes, length)) return -1;
        if (memcmp(bytes, name, length)) continue;
        if (!process_memory_read(entries[i][0], out->fields, sizeof(out->fields))) return -1;
        uintptr_t datatype = (uintptr_t)ref(out->fields, 0x70);
        if (!process_memory_read(datatype + 0x30, &out->type, sizeof(out->type))) return -1;
        out->descriptor = entries[i][0];
        out->instance = instance;
        return 1;
    }
    return 0;
}
static int property_argument(void* L, struct reflected_property* property)
{
    uintptr_t instance = (uintptr_t)ref(instance_arg(L, 1), 0);
    Value name = arg(L, 2);
    if (name.tag != 6) return fail(L, "expected a property name string");
    size_t length = *(uint32_t*)ptr((void*)name.value, 0x14);
    int found = reflection_lookup(instance, (const char*)ptr((void*)name.value, 0x18), length, property);
    if (found < 0) return fail(L, "property reflection layout is unavailable");
    return found;
}
static int property_default(void* L)
{
    struct reflected_property property;
    if (!property_argument(L, &property)) push(L, (Value){0});
    else g_engine.lua_pushboolean(L, !!(property.fields[VM_PROPERTY_FLAGS] & 0x10));
    return 1;
}
static int property_descriptors(void* L)
{
    uintptr_t instance = (uintptr_t)ref(instance_arg(L, 1), 0), klass = 0, properties[2], entries[512][2];
    if (!process_memory_read(instance + 0x18, &klass, 8) ||
        !process_memory_read(klass + 0x40, properties, sizeof(properties)) || properties[1] > 512 ||
        (properties[1] && !process_memory_read(properties[0], entries, properties[1] * sizeof(entries[0]))))
        return fail(L, "property reflection layout is unavailable");
    void* result = g_engine.new_table(L, (int)properties[1], 0);
    push(L, object(result, 7));
    for (size_t i = 0; i < properties[1]; ++i) {
        uintptr_t name = 0, datatype = 0;
        uint64_t storage[3];
        uint32_t type = 0;
        uint8_t flags = 0;
        char bytes[256];
        if (!process_memory_read(entries[i][0] + 8, &name, 8) ||
            !process_memory_read(name, storage, sizeof(storage)) ||
            !process_memory_read(entries[i][0] + 0x70, &datatype, 8) ||
            !process_memory_read(datatype + 0x30, &type, 4) ||
            !process_memory_read(entries[i][0] + 0x93, &flags, 1))
            return fail(L, "property descriptor is unavailable");
        size_t length = storage[0] & 1 ? storage[1] : *(uint8_t*)storage >> 1;
        uintptr_t address = storage[0] & 1 ? storage[2] : name + 1;
        if (!length || length >= sizeof(bytes) || !process_memory_read(address, bytes, length))
            return fail(L, "property name is unavailable");
        void* row = g_engine.new_table(L, 0, 3);
        push(L, object(row, 7));
        named(L, row, "Name", object(g_engine.new_string(L, bytes, length), 6));
        named(L, row, "Type", number(type));
        named(L, row, "Scriptable", (Value){.tag=1, .value=!!(flags & 0x10)});
        put(L, result, number(i + 1), object(row, 7));
        *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    }
    return 1;
}
static int instance_key(void* L)
{
    uintptr_t address = (uintptr_t)ref(instance_arg(L, 1), 0);
    char key[32];
    snprintf(key, sizeof(key), "%016llx", (unsigned long long)address);
    g_engine.lua_pushstring(L, key);
    return 1;
}
static int queue_actor(void* L)
{
    uintptr_t* shared = instance_arg(L, 1);
    if (!instance_class_named(shared[0], "Actor")) return fail(L, "expected an Actor");
    Value code = arg(L, 2);
    if (code.tag != 6) return fail(L, "run_on_actor expects Luau source");
    size_t length = *(uint32_t*)ptr((void*)code.value, 0x14);
    if (!length || length > 8u * 1024u * 1024u) return fail(L, "Actor source must be 1 byte to 8 MiB");
    size_t encoded_size=0;
    char* encoded=compile_encoded((const char*)ptr((void*)code.value,0x18),length,&encoded_size);
    if (!encoded) return fail(L,"Actor compiler allocation failed");
    if (!encoded_size || !encoded[0]) {
        char message[384]="Actor source failed to compile";
        if (encoded_size>1) {
            size_t copy=encoded_size-1<sizeof(message)-1 ? encoded_size-1 : sizeof(message)-1;
            memcpy(message,encoded+1,copy);message[copy]=0;
        }
        free(encoded);
        return fail(L,message);
    }
    uint32_t id = 0;
    int accepted=executor_actor_enqueue(shared,encoded,encoded_size,&id);
    free(encoded);
    if (!accepted)
        return fail(L, "Actor queue is full or the Actor has expired");
    push(L, number(id));
    return 1;
}
static int poll_actor(void* L, int cancel)
{
    uint32_t id = (uint32_t)integer(L, 1, 1);
    char error[160] = {0};
    int status = 0;
    if (!executor_actor_poll(id, &status, error, sizeof(error), cancel))
        return fail(L, "Actor job is unavailable");
    if (!status) push(L, (Value){0});
    else g_engine.lua_pushboolean(L, status == 1);
    if (status == -1) g_engine.lua_pushstring(L, error[0] ? error : "Actor scheduling failed");
    return status == -1 ? 2 : 1;
}
static int poll_actor_normal(void* L) { return poll_actor(L, 0); }
static int poll_actor_cancel(void* L) { return poll_actor(L, 1); }
static int engine_function(uintptr_t function)
{
    return function >= g_engine.text_start && function - g_engine.text_start < g_engine.text_size;
}
static uintptr_t reflection_virtual(void* L, uintptr_t object_address, size_t offset)
{
    uintptr_t vtable = 0, function = 0;
    if (!process_memory_read(object_address, &vtable, sizeof(vtable)) ||
        !process_memory_read(vtable + offset, &function, sizeof(function)) || !engine_function(function))
    { fail(L, "unsupported property accessor"); return 0; }
    return function;
}
struct property_operation {
    struct reflected_property property;
    uint64_t variant[10];
    int write;
};
static void push_reflection_string(void* L, const void* string)
{
    uint64_t storage[3];
    memcpy(storage, string, sizeof(storage));
    size_t length = (storage[0] & 1) ? storage[1] : *(uint8_t*)storage >> 1;
    const char* bytes = (storage[0] & 1) ? (const char*)(uintptr_t)storage[2] : (const char*)string + 1;
    if (length > 16 * 1024 * 1024 || (length && !bytes)) { fail(L, "invalid reflected string"); return; }
    g_engine.lua_pushlstring(L, bytes, length);
}
static void reflection_operation(void* L, void* opaque)
{
    struct property_operation* operation = opaque;
    struct reflected_property* property = &operation->property;
    if (operation->write)
    {
        if (property->type == 30)
        {
            Value value = arg(L, 3);
            if (value.tag != 6) { fail(L, "expected a string for BinaryString property"); return; }
            size_t length = *(uint32_t*)ptr((void*)value.value, 0x14);
            // Borrow the rooted Luau bytes as a const libc++ string; the setter copies them.
            uint64_t string[3] = {(length + 1) | 1, length, value.value + 0x18};
            uintptr_t accessor = (uintptr_t)ref(property->fields, VM_PROPERTY_ACCESSOR);
            void (*set)(void*, void*, const void*) = (void*)reflection_virtual(L, accessor, 0x28);
            set((void*)accessor, (void*)property->instance, string);
        }
        else
        {
            Value incoming = arg(L, 3);
            if (property->type >= 2 && property->type <= 5 && incoming.tag != 3)
            { fail(L, "expected a number for numeric property"); return; }
            if (property->type == 1 && incoming.tag != 1)
            { fail(L, "expected a boolean for boolean property"); return; }
            // Native assignment reads the fixed descriptor fields. A stack-local
            // copy changes scriptability without touching the class descriptor.
            property->fields[VM_PROPERTY_FLAGS] |= 0x10;
            uintptr_t pair[2] = {(uintptr_t)property->fields, property->instance};
            g_engine.write_property(pair, L, g_engine.security_context(L));
        }
        return;
    }
    void (*get)(void*, void*, void*) = (void*)reflection_virtual(L, property->descriptor, 0x88);
    get((void*)property->descriptor, (void*)property->instance, operation->variant);
    if (!operation->variant[0]) { fail(L, "property getter returned no reflected type"); return; }
    uint32_t type = 0;
    if (!process_memory_read(operation->variant[0] + 0x30, &type, sizeof(type)))
    { fail(L, "property value type is unreadable"); return; }
    if (type == 29)
    {
        push(L, number(operation->variant[2] == UINT32_MAX ? -1 :
            (double)(int64_t)operation->variant[2]));
    }
    else if (type == 30) push_reflection_string(L, &operation->variant[2]);
    else if (type == 63)
    {
        uint64_t string[3];
        if (!process_memory_read(operation->variant[2] + VM_SHAREDSTRING_DATA, string, sizeof(string)))
        { fail(L, "SharedString storage is unreadable"); return; }
        push_reflection_string(L, string);
    }
    else if (g_engine.push_variant(operation->variant, L) != 1)
        fail(L, "property did not return exactly one value");
}
static int property_access(void* L, int write)
{
    struct property_operation operation = {.write=write};
    if (!property_argument(L, &operation.property)) return fail(L, "property does not exist");
    int hidden = !(operation.property.fields[VM_PROPERTY_FLAGS] & 0x10);
    ptrdiff_t oldtop = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    int status = g_engine.luaD_pcall(L, reflection_operation, &operation, oldtop, 0);
    // Variant's small-buffer storage owns strings/shared references, even when
    // pushing the result raised a Lua error. Release it outside the protected call.
    if (operation.variant[1])
    {
        uintptr_t destructor = 0;
        if (process_memory_read(operation.variant[1] + 0x10, &destructor, sizeof(destructor)) && engine_function(destructor))
            ((void (*)(void*))destructor)(&operation.variant[2]);
    }
    if (status) { g_engine.lua_error(L); return 0; }
    g_engine.lua_pushboolean(L, hidden);
    return write ? 1 : 2;
}
static int read_property(void* L) { return property_access(L, 0); }
static int write_property(void* L) { return property_access(L, 1); }
static int instance_class_named(uintptr_t instance, const char* expected)
{
    uintptr_t descriptor = 0, name = 0;
    uint8_t text[24];
    size_t length = strlen(expected);
    return length < sizeof(text) && process_memory_read(instance + 0x18, &descriptor, 8) &&
        process_memory_read(descriptor + 8, &name, 8) && process_memory_read(name, text, sizeof(text)) &&
        !(text[0] & 1) && (text[0] >> 1) == length && !memcmp(text + 1, expected, length);
}
static void prompt_protected(void* L, void* prompt)
{
    (void)L;
    g_engine.trigger_prompt(prompt);
}
static int fire_proximity_prompt(void* L)
{
    void* prompt = ref(instance_arg(L, 1), 0);
    if (!instance_class_named((uintptr_t)prompt, "ProximityPrompt")) return fail(L, "expected a ProximityPrompt");
    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    if (g_engine.luaD_pcall(L, prompt_protected, prompt, saved, 0)) g_engine.lua_error(L);
    return 0;
}
struct click_operation { void* detector; uintptr_t player[2]; float distance; int event; };
static void click_protected(void* L, void* context)
{
    (void)L;
    struct click_operation* op = context;
#if !BASALT_ARM_744
    if (op->event == 0) g_engine.click_detector(op->detector, op->distance, (void*)op->player[0]);
    else if (op->event == 3) g_engine.right_click_detector(op->detector, op->distance, (void*)op->player[0]);
    else
#endif
    {
        // The engine's hover dispatch (0x50c9d..-0x50c9f..) reads the signal member
        // offset from a runtime-initialised global and passes the descriptor at
        // +0x6A25AC0 to the replication routine.
        uint64_t signal_offset = 0;
        uintptr_t offset_address, descriptor;
#if BASALT_ARM_744
        // Verified in the four branches at 0x51b1730..0x51b17f4.
        static const uintptr_t offsets[] = {0x06c51970, 0x06c519f8, 0x06c51a80, 0x06c51b08};
        offset_address = offsets[op->event];
        descriptor = 0x06c51858;
#else
        offset_address = op->event == 1 ? 0x06AC2138 : 0x06AC21C0;
        descriptor = 0x06AC1F98;
#endif
        if (!process_memory_read(g_engine.base + offset_address, &signal_offset, 8) ||
            signal_offset < 0x40 || signal_offset > 0x400) return;
        g_engine.signal_player(ptr(op->detector, signal_offset), op->player);
        uintptr_t player[2] = {op->player[0], op->player[1]};
        __atomic_fetch_add((int64_t*)(player[1] + 8), 1, __ATOMIC_RELAXED);
        g_engine.replicate_click((void*)(g_engine.base + descriptor), op->detector, player, op->event);
    }
}
static int fire_click_detector(void* L)
{
    struct click_operation op = {.detector=ref(instance_arg(L, 1), 0)};
    memcpy(op.player, instance_arg(L, 2), sizeof(op.player));
    if (!instance_class_named((uintptr_t)op.detector, "ClickDetector") ||
        !instance_class_named(op.player[0], "Player") || !op.player[1])
        return fail(L, "expected a ClickDetector and Player");
    Value value = arg(L, 3);
    double distance;
    memcpy(&distance, &value.value, 8);
    if (value.tag != 3 || !isfinite(distance) || distance < 0 || distance > 1e9)
        return fail(L, "invalid click distance");
    op.distance = (float)distance;
    op.event = integer(L, 4, 0);
    if (op.event < 0 || op.event > 3) return fail(L, "unsupported ClickDetector event");
    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    if (g_engine.luaD_pcall(L, click_protected, &op, saved, 0)) g_engine.lua_error(L);
    return 0;
}
static int instance_is_class(uintptr_t instance, uintptr_t expected)
{
    uintptr_t actual = 0;
    uint16_t child = 0, first = 0, span = 0;
    return process_memory_read(instance + 0x18, &actual, 8) &&
        process_memory_read(actual + 0x1b8, &child, 2) &&
        process_memory_read(expected + 0x1b8, &first, 2) &&
        process_memory_read(expected + 0x1ba, &span, 2) && child >= first && child - first <= span;
}
struct touch_operation { uintptr_t first, other[2]; int ended; };
static void touch_protected(void* L, void* context)
{
    (void)L;
    struct touch_operation* op = context;
    g_engine.touch_part((void*)op->first, op->other, op->ended, 1);
}
static int fire_touch_interest(void* L)
{
    uintptr_t shared[2][2];
    memcpy(shared[0], instance_arg(L, 1), 16);
    memcpy(shared[1], instance_arg(L, 2), 16);
    if (!instance_is_class(shared[0][0], g_engine.base + VM_BASEPART_DESCRIPTOR) ||
        !instance_is_class(shared[1][0], g_engine.base + VM_BASEPART_DESCRIPTOR))
        return fail(L, "expected two BaseParts");
    Value toggle = arg(L, 3);
    int ended;
    if (toggle.tag == 1) ended = !(uint32_t)toggle.value;
    else if (toggle.tag == 3) {
        double value;
        memcpy(&value, &toggle.value, 8);
        if (value != 0 && value != 1) return fail(L, "touch toggle must be boolean, zero, or one");
        ended = (int)value;
    } else return fail(L, "touch toggle must be boolean, zero, or one");
    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    for (unsigned i = 0; i < 2; ++i) {
        struct touch_operation op = {shared[i][0], {shared[1-i][0], shared[1-i][1]}, ended};
        if (g_engine.luaD_pcall(L, touch_protected, &op, saved, 0)) { g_engine.lua_error(L); return 0; }
    }
    return 0;
}
struct variant_conversion { uint64_t* value; int index; };
#if BASALT_ARM_744
// Scalar Variant managers contain copy, move, and destroy callbacks. The
// 2.744 double manager at 0x1fd4504 constructs this exact three-pointer ABI;
// scalar storage is inline and requires no destructor work.
static void scalar_copy32(const void* source, void* destination) { memcpy(destination, source, 4); }
static void scalar_copy64(const void* source, void* destination) { memcpy(destination, source, 8); }
static void scalar_destroy(void* value) { (void)value; }
static const struct {
    void (*copy)(const void*, void*);
    void (*move)(const void*, void*);
    void (*destroy)(void*);
} scalar_variant_managers[] = {
    {scalar_copy32, scalar_copy32, scalar_destroy},
    {scalar_copy64, scalar_copy64, scalar_destroy},
};
#endif
static void variant_conversion_protected(void* L, void* context)
{
    struct variant_conversion* op = context;
    g_engine.read_variant(L, op->index, op->value, 1);
}
struct signal_replication { uintptr_t instance, descriptor, arguments[3], address[2], function; };
static int reflection_name(uintptr_t address, char* text, size_t capacity, size_t* length)
{
    uint64_t storage[3];
    if (!process_memory_read(address, storage, sizeof(storage))) return 0;
    *length = storage[0] & 1 ? storage[1] : *(uint8_t*)storage >> 1;
    uintptr_t data = storage[0] & 1 ? storage[2] : address + 1;
    return *length > 0 && *length < capacity && process_memory_read(data, text, *length);
}
static int signal_arguments_info(void* L)
{
    Value signal = arg(L, 1);
    uintptr_t binding = 0, descriptor = 0, specs[2];
    if (signal.tag != 9 || *ptr((void*)signal.value, 3) != 0x2f ||
        *(uint32_t*)ptr((void*)signal.value, 4) != 40 ||
        !process_memory_read(signal.value + 0x10, &binding, 8) ||
        !process_memory_read(binding, &descriptor, 8))
        return fail(L, "expected an RBXScriptSignal");
    if (!process_memory_read(descriptor + VM_EVENT_ARGUMENTS, specs, sizeof(specs)) || specs[1] < specs[0] ||
        (specs[1] - specs[0]) % 0x70 || (specs[1] - specs[0]) / 0x70 > 32)
        return fail(L, "signal argument signature is unavailable");
    unsigned count = (specs[1] - specs[0]) / 0x70;
    void* result = g_engine.new_table(L, count, 0);
    push(L, object(result, 7));
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t name = 0, datatype = 0, type_name = 0;
        char argument[256], type[256];
        size_t argument_length, type_length;
        if (!process_memory_read(specs[0] + i * 0x70, &name, 8) ||
            !process_memory_read(specs[0] + i * 0x70 + 8, &datatype, 8) ||
            !process_memory_read(datatype + 8, &type_name, 8) ||
            !reflection_name(name, argument, sizeof(argument), &argument_length) ||
            !reflection_name(type_name, type, sizeof(type), &type_length))
            return fail(L, "signal argument names are unavailable");
        void* row = g_engine.new_table(L, 0, 2);
        push(L, object(row, 7));
        named(L, row, "Name", object(g_engine.new_string(L, argument, argument_length), 6));
        named(L, row, "Type", object(g_engine.new_string(L, type, type_length), 6));
        put(L, result, number(i + 1), object(row, 7));
        *(Value**)ptr(L, VM_L_TOP) = top(L) - 1;
    }
    return 1;
}
static void signal_replicate_protected(void* L, void* context)
{
    (void)L;
    struct signal_replication* op = context;
    ((void(*)(void*, void*, const void*, const void*))op->function)
        ((void*)op->instance, (void*)op->descriptor, op->arguments, op->address);
}
static int replicate_signal(void* L)
{
    Value signal = arg(L, 1);
    uintptr_t binding = 0, descriptor = 0, instance = 0, control = 0;
    // RBXScriptSignal owns a binding containing its EventDescriptor and a weak
    // Instance reference. Keep that Instance alive during native replication.
    if (signal.tag != 9 || *ptr((void*)signal.value, 3) !=
#if BASALT_ARM_744
        0x2f ||
#else
        0x37 ||
#endif
        *(uint32_t*)ptr((void*)signal.value, 4) != 40 ||
        !process_memory_read(signal.value + 0x10, &binding, 8) ||
        !process_memory_read(binding, &descriptor, 8) ||
        !process_memory_read(binding + 0x18, &instance, 8) ||
        !process_memory_read(binding + 0x20, &control, 8) || !instance || !control)
        return fail(L, "expected a live RBXScriptSignal");
    uintptr_t specs[2];
    if (!process_memory_read(descriptor + VM_EVENT_ARGUMENTS, specs, sizeof(specs)) || specs[1] < specs[0] ||
        (specs[1] - specs[0]) % 0x70 || (specs[1] - specs[0]) / 0x70 > 32)
        return fail(L, "signal argument signature is unavailable");
    unsigned count = (specs[1] - specs[0]) / 0x70;
    if (g_engine.lua_gettop(L) != (int)count + 1) return fail(L, "signal argument count does not match");
    uint64_t variants[32][10] = {{0}};
    const char* error = NULL;
    int status = 0, held = 0;
    ptrdiff_t saved = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t datatype = 0, constraint = 0;
        uint32_t type = 0;
        if (!process_memory_read(specs[0] + i * 0x70 + 8, &datatype, 8) ||
            !process_memory_read(datatype + 0x30, &type, 4) ||
            !process_memory_read(specs[0] + i * 0x70 + 0x18, &constraint, 8)) {
            error = "signal argument metadata is unavailable"; break;
        }
        Value value = arg(L, i + 2);
        if (type >= 2 && type <= 5) {
            if (value.tag != 3) { error = "expected a numeric signal argument"; break; }
            double n;
            memcpy(&n, &value.value, 8);
            variants[i][0] = datatype;
            // Canonical scalar managers used by the pinned Variant constructors.
#if BASALT_ARM_744
            variants[i][1] = (uintptr_t)&scalar_variant_managers[type == 2 || type == 4 ? 0 : 1];
#else
            static const uintptr_t managers[] = {0x01E73E24, 0x02C12D2C, 0x0601E4AC, 0x01F35FF0};
            variants[i][1] = ((uintptr_t(*)(void))(g_engine.base + managers[type - 2]))();
#endif
            // Keep the Linux API's integer conversion on both architectures:
            // truncate fractions; NaN, infinity and overflow become INT_MIN.
            // Explicit bounds avoid undefined out-of-range C casts on ARM64.
            if (type == 2) variants[i][2] = (uint32_t)(!isfinite(n) || n >= 0x1p31 || n < -0x1p31 ?
                INT32_MIN : (int32_t)n);
            else if (type == 3) variants[i][2] = (uint64_t)(!isfinite(n) || n >= 0x1p63 || n < -0x1p63 ?
                INT64_MIN : (int64_t)n);
            else if (type == 4) { float f = (float)n; memcpy(&variants[i][2], &f, 4); }
            else memcpy(&variants[i][2], &n, 8);
        } else {
            if (type == 1 && value.tag != 1) { error = "expected a boolean signal argument"; break; }
            if (type == 6 && value.tag != 6) { error = "expected a string signal argument"; break; }
            if (type == 51) {
                uint8_t tag =
#if BASALT_ARM_744
                    0x3b;
#else
                    *(uint32_t*)(g_engine.base + 0x06C32098);
#endif
                if (value.tag != 9 || *ptr((void*)value.value, 3) != tag ||
                    *(uint32_t*)ptr((void*)value.value, 4) != 16 ||
                    (constraint && !instance_is_class((uintptr_t)ref((void*)value.value, 0x10), constraint))) {
                    error = "Instance signal argument has the wrong class"; break;
                }
            }
            struct variant_conversion op = {variants[i], (int)i + 2};
            status = g_engine.luaD_pcall(L, variant_conversion_protected, &op, saved, 0);
            if (status) break;
            uint32_t converted = 0;
            if (!variants[i][0] || !process_memory_read(variants[i][0] + 0x30, &converted, 4) || converted != type) {
                error = "signal argument type does not match"; break;
            }
        }
    }
    uintptr_t shared[2] = {instance, control};
    if (!status && !error) {
        int64_t owners = __atomic_load_n((int64_t*)(control + 8), __ATOMIC_ACQUIRE);
        while (owners >= 0 && owners < INT64_MAX) {
            if (__atomic_compare_exchange_n((int64_t*)(control + 8), &owners, owners + 1, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { held = 1; break; }
        }
        if (!held) error = "signal Instance has expired";
        else {
            struct signal_replication op = {.instance=instance, .descriptor=descriptor,
                .arguments={(uintptr_t)variants, (uintptr_t)(variants + count), (uintptr_t)(variants + count)}};
            uintptr_t vtable = 0;
            if (!process_memory_read(instance, &vtable, 8) ||
                !process_memory_read(vtable + 0x20, &op.function, 8) || !engine_function(op.function))
                error = "signal replication dispatcher is unavailable";
            else status = g_engine.luaD_pcall(L, signal_replicate_protected, &op, saved, 0);
        }
    }
    if (held) g_engine.release_shared(shared);
    for (unsigned i = 0; i < count; ++i) if (variants[i][1]) {
        uintptr_t destructor = 0;
        if (process_memory_read(variants[i][1] + 0x10, &destructor, 8) && engine_function(destructor))
            ((void(*)(void*))destructor)(variants[i] + 2);
    }
    if (status) { g_engine.lua_error(L); return 0; }
    return error ? fail(L, error) : 0;
}
static int callback_value(void* L)
{
    uintptr_t instance = (uintptr_t)ref(instance_arg(L, 1), 0);
    uintptr_t active = 0, context = 0, owner = 0, record = 0, thread = 0, global = 0;
    uint32_t index = 0;
    if (!process_memory_read(instance + 0xf0, &active, sizeof(active)))
        return fail(L, "callback storage is not readable");
    if (!active) { push(L, (Value){0}); return 1; }
    // Pinned std::function -> Lua callback context -> registry reference.
    if (active != instance + 0xd0 ||
        !process_memory_read(instance + 0xe0, &context, sizeof(context)) ||
        !process_memory_read(context + 0x20, &owner, sizeof(owner)) ||
        !process_memory_read(owner + 0x18, &record, sizeof(record)) ||
        !process_memory_read(record + 0x28, &thread, sizeof(thread)) ||
        !process_memory_read(thread + VM_L_GLOBAL, &global, sizeof(global)) ||
        global != (uintptr_t)ref(L, VM_L_GLOBAL) ||
        !process_memory_read(record + 0x34, &index, sizeof(index)))
        return fail(L, "callback belongs to an inaccessible state or unsupported layout");
    Value key = number(index);
    getreg(L);
    Value registry = top(L)[-1];
    if (registry.tag != 7) return fail(L, "callback registry is unavailable");
    Value value = *(const Value*)g_engine.table_get((void*)registry.value, &key);
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    if (value.tag != 8) return fail(L, "callback registry reference is unavailable");
    push(L, value);
    return 1;
}
struct connection_snapshot {
    uintptr_t node;
    int lua, foreign, once, waiting;
    Value function, thread;
};
static int connection_lua_slot(int fd, uintptr_t node, uintptr_t* slot, uintptr_t* control)
{
    uintptr_t destructor = 0, pair[2] = {0};
    if (!read_memory(fd, node + 0x28, &destructor, sizeof(destructor))) return 0;
    if (destructor == g_engine.base + VM_CONNECTION_LUA_DESTRUCTOR)
    {
        if (!read_memory(fd, node + 0x30, pair, sizeof(pair))) return 0;
    }
    else if (destructor == g_engine.base + VM_CONNECTION_REFLECTED_DESTRUCTOR)
    {
        // Generic reflected events (including Touched) capture a shared Lua slot
        // inside std::function rather than storing it directly in the signal node.
        uintptr_t functor = 0, vtable = 0;
        if (!read_memory(fd, node + 0x50, &functor, sizeof(functor)) ||
            !read_memory(fd, functor + 8, pair, sizeof(pair)) ||
            !read_memory(fd, pair[0], &vtable, sizeof(vtable)) ||
            (vtable != g_engine.base + VM_CONNECTION_LUA_VTABLE && vtable != g_engine.base + VM_CONNECTION_WAIT_VTABLE))
            return 0;
    }
    else return 0;
    if (!pair[0] || !pair[1]) return 0;
    *slot = pair[0];
    *control = pair[1];
    return 1;
}
static void push_connection_handle(void* L, uintptr_t node)
{
#if BASALT_ARM_744
    uint64_t* handle = g_engine.new_userdata(L, 24, 8);
#else
    void* (*new_userdata)(void*, const char*, int, int) = (void*)(g_engine.base + 0x025559B0);
    const char* name = *(const char**)(g_engine.base + 0x06AB3EE8);
    int tag = 8;
    uint64_t* handle = new_userdata(L, name, 24, tag);
#endif
    handle[0] = handle[1] = handle[2] = 0;
    // Engine userdata holds a weak intrusive node reference; its existing finalizer releases it.
    __atomic_fetch_add((uint32_t*)(node + 4), 1u, __ATOMIC_RELAXED);
    handle[0] = node;
    uintptr_t slot = 0, control = 0;
    if (connection_lua_slot(-1, node, &slot, &control))
    {
        __atomic_fetch_add((uint64_t*)(control + 0x10), UINT64_C(1), __ATOMIC_RELAXED);
        handle[1] = slot + 8;
        handle[2] = control;
    }
}
static void release_snapshot_nodes(struct connection_snapshot* snapshots, unsigned count)
{
    // We only add to a live node's strong count, so dropping our extra reference never frees it.
    for (unsigned i = 0; i < count; ++i) __atomic_fetch_sub((uint32_t*)snapshots[i].node, 1u, __ATOMIC_RELAXED);
}
struct snapshot_output {
    struct connection_snapshot* items;
    unsigned count;
};
static void build_snapshot_output(void* L, void* context)
{
    struct snapshot_output* snapshot = context;
    void* output = g_engine.new_table(L, snapshot->count, 0);
    push(L, object(output, 7));
    for (unsigned i = 0; i < snapshot->count; ++i)
    {
        struct connection_snapshot* item = &snapshot->items[i];
        void* entry = g_engine.new_table(L, 0, 10);
        push(L, object(entry, 7));
        named(L, entry, "NativeAddress", number(item->node));
        named(L, entry, "LuaConnection", (Value){.tag=1, .value=item->lua});
        named(L, entry, "ForeignState", (Value){.tag=1, .value=item->foreign});
        named(L, entry, "Function", item->function);
        named(L, entry, "Thread", item->thread);
        named(L, entry, "Once", (Value){.tag=1, .value=item->once});
        named(L, entry, "Waiting", (Value){.tag=1, .value=item->waiting});
        push_connection_handle(L, item->node);
        named(L, entry, "Handle", top(L)[-1]);
        *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
        put(L, output, number(i + 1), object(entry, 7));
        *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    }
}
static int snapshot_connections(void* L)
{
    uintptr_t temporary = (uintptr_t)ref(connection_arg(L, 1), 0);
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    uintptr_t head = 0, current = 0;
    if (!read_memory(fd, temporary + 0x20, &head, 8) ||
        !read_memory(fd, head + 8, &current, 8))
    { close(fd); return fail(L, "temporary connection has no signal list"); }
    struct connection_snapshot* snapshots = calloc(4096, sizeof(*snapshots));
    if (!snapshots) { close(fd); return fail(L, "connection snapshot allocation failed"); }
    unsigned count = 0;
    const char* error = NULL;
    if (!read_memory(fd, head + 8, &current, 8)) error = "signal head is no longer readable";
    while (current && !error)
    {
        uint64_t node[8];
        if (count == 4096) { error = "connection list exceeds inspection limit"; break; }
        if (!read_memory(fd, current, node, sizeof(node)) || node[4] != head)
        { error = "connection list changed during inspection"; break; }
        if (current != temporary)
        {
            for (unsigned i = 0; i < count; ++i)
                if (snapshots[i].node == current) { error = "cyclic connection list"; break; }
            if (error) break;
            struct connection_snapshot* item = &snapshots[count++];
            item->node = current;
            // Hold a strong intrusive reference across subsequent VM allocations/GC.
            __atomic_fetch_add((uint32_t*)current, 1u, __ATOMIC_RELAXED);
            item->foreign = 1;
            uintptr_t slot_pointer = 0, slot_control = 0;
            if (connection_lua_slot(fd, current, &slot_pointer, &slot_control))
            {
                uint64_t slot[21] = {0}, record[7], global = 0;
                item->lua = 1;
                uintptr_t record_pointer = 0;
                if (read_memory(fd, slot_pointer, slot, 8))
                {
                    if (slot[0] == g_engine.base + VM_CONNECTION_LUA_VTABLE && read_memory(fd, slot_pointer, slot, sizeof(slot)))
                    {
                        if ((uint32_t)slot[10] == 0) record_pointer = slot[13];
                        item->once = ((uint8_t*)slot)[0xa5] != 0;
                    }
                    else if (slot[0] == g_engine.base + VM_CONNECTION_WAIT_VTABLE && read_memory(fd, slot_pointer, slot, 0x78))
                    {
                        item->waiting = 1;
                        record_pointer = slot[7];
                    }
                }
                if (read_memory(fd, record_pointer, record, sizeof(record)) && record[2] &&
                    read_memory(fd, record[5] + VM_L_GLOBAL, &global, 8) && global == (uintptr_t)ref(L, VM_L_GLOBAL))
                {
                    getreg(L);
                    Value registry_value = top(L)[-1];
                    void* registry = registry_value.tag == 7 ? (void*)registry_value.value : NULL;
                    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
                    if (!registry) { error = "connection registry is unavailable"; break; }
                    Value thread_key = number((uint32_t)record[6]);
                    Value function_key = number((uint32_t)(record[6] >> 32));
                    item->thread = *(Value*)g_engine.table_get(registry, &thread_key);
                    if (!item->waiting) item->function = *(Value*)g_engine.table_get(registry, &function_key);
                    if (item->thread.tag == 10 && (item->function.tag == 8 || item->waiting))
                        item->foreign = 0;
                    else item->thread = item->function = (Value){0};
                }
            }
        }
        if (node[2] == current) { error = "cyclic connection list"; break; }
        current = node[2];
    }
    close(fd);
    if (error) { release_snapshot_nodes(snapshots, count); free(snapshots); return fail(L, error); }
    // Root callbacks/threads in the returned table before any collection can run.
    size_t* threshold = (size_t*)ptr(ref(L, VM_L_GLOBAL), VM_GLOBAL_GC_THRESHOLD);
    size_t saved_threshold = *threshold;
    *threshold = SIZE_MAX;
    struct snapshot_output context = {snapshots, count};
    ptrdiff_t oldtop = (uint8_t*)top(L) - (uint8_t*)ref(L, 0x60);
    int status = g_engine.luaD_pcall(L, build_snapshot_output, &context, oldtop, 0);
    *threshold = saved_threshold;
    release_snapshot_nodes(snapshots, count);
    free(snapshots);
    if (status) { g_engine.lua_error(L); return 0; }
    return 1;
}
static void muted_signal_callback(void) {}
static int set_connection_enabled(void* L)
{
    void* handle = connection_arg(L, 1);
    Value enabled = arg(L, 2);
    if (enabled.tag != 1) return fail(L, "expected a boolean");
    uintptr_t node = (uintptr_t)ref(handle, 0);
    if (!node) return 0;
    void* map = registry_map(L, "__basalt_muted_connections_v1");
    Value key = number(node);
    uintptr_t* callback = (uintptr_t*)(node + 8);
    uintptr_t current = __atomic_load_n(callback, __ATOMIC_RELAXED);
    Value saved = *(Value*)g_engine.table_get(map, &key);
    if ((uint32_t)enabled.value)
    {
        if (current == (uintptr_t)muted_signal_callback && saved.tag == 2)
            __atomic_store_n(callback, (uintptr_t)saved.value, __ATOMIC_RELAXED);
        put(L, map, key, (Value){0});
    }
    else if (current != (uintptr_t)muted_signal_callback)
    {
        // The signal invoker has a void return; extra ABI arguments are ignored by this no-op.
        put(L, map, key, (Value){.tag=2, .value=current});
        __atomic_store_n(callback, (uintptr_t)muted_signal_callback, __ATOMIC_RELAXED);
    }
    return 0;
}
static int connection_enabled(void* L)
{
    void* handle = connection_arg(L, 1);
    uintptr_t node = (uintptr_t)ref(handle, 0);
    int enabled = 0;
    if (node)
    {
        enabled = __atomic_load_n((uintptr_t*)(node + 8), __ATOMIC_RELAXED) != (uintptr_t)muted_signal_callback;
    }
    g_engine.lua_pushboolean(L, enabled);
    return 1;
}
static void** metatable_slot(void* L, Value value)
{
    if (value.tag == 7) return (void**)ptr((void*)value.value,
#if BASALT_ARM_744
        VM_TABLE_METATABLE
#else
        0x20
#endif
    );
    if (value.tag == 9) return (void**)ptr((void*)value.value, 8);
    if (value.tag >= 12) { fail(L, "unsupported value type"); return NULL; }
    return (void**)ptr(ref(L, 0x50), 0x440 + value.tag * 8);
}
static int getrawmetatable(void* L)
{
    if (!g_engine.lua_gettop(L)) return fail(L, "expected a value");
    void* mt = *metatable_slot(L, arg(L, 1));
    push(L, mt ? object(mt, 7) : (Value){0});
    return 1;
}
static int setrawmetatable(void* L)
{
    if (g_engine.lua_gettop(L) < 2) return fail(L, "expected a value and metatable");
    Value value = arg(L, 1), mt = arg(L, 2);
    if (mt.tag && mt.tag != 7) return fail(L, "metatable must be a table or nil");
    *metatable_slot(L, value) = mt.tag == 7 ? (void*)mt.value : NULL;
    if (value.tag == 7 || value.tag == 9) barrier(L, (void*)value.value, mt);
    push(L, value);
    return 1;
}
static int isreadonly(void* L)
{
    Value value = arg(L, 1);
    if (value.tag != 7) return fail(L, "expected a table");
    g_engine.lua_pushboolean(L, *ptr((void*)value.value,
#if BASALT_ARM_744
        7
#else
        4
#endif
    ) != 0);
    return 1;
}
static int debug_setname(void* L)
{
    void* closure = function_arg(L, 1, 1);
    Value name = arg(L, 2);
    if (name.tag != 6) return fail(L, "expected a name string");
    void* owner = *ptr(closure, 3) ? closure : ref(closure, 0x18);
    size_t offset = *ptr(closure, 3) ? VM_CCLOSURE_DEBUGNAME : 0x60;
    *(void**)ptr(owner, offset) = (void*)name.value;
    barrier(L, owner, name);
    return 0;
}
static int debug_setinfo(void* L)
{
    void* proto = ref(function_arg(L, 1, 0), 0x18);
    Value fields = arg(L, 2);
    if (fields.tag != 7) return fail(L, "expected a metadata table");
    const char* names[] = {"name", "source", "currentline"};
    Value values[3];
    for (unsigned i = 0; i < 3; ++i) {
        Value key = object(g_engine.new_string(L, names[i], strlen(names[i])), 6);
        values[i] = *(Value*)g_engine.table_get((void*)fields.value, &key);
        if (values[i].tag && values[i].tag != (i == 2 ? 3u : 6u)) return fail(L, "invalid metadata field type");
    }
    double line = 0;
    if (values[2].tag) {
        memcpy(&line, &values[2].value, sizeof(line));
        if (!isfinite(line) || floor(line) != line || line < 0 || line > INT32_MAX)
            return fail(L, "invalid metadata line");
    }
    for (unsigned i = 0; i < 2; ++i) if (values[i].tag) {
        *(void**)ptr(proto, i == 0 ? 0x60 : 0x08) = (void*)values[i].value;
        barrier(L, proto, values[i]);
    }
    if (values[2].tag) *(int32_t*)ptr(proto, 0x90) = (int32_t)line;
    return 0;
}
static int setreadonly(void* L)
{
    Value value = arg(L, 1), state = arg(L, 2);
    if (value.tag != 7 || state.tag != 1) return fail(L, "expected a table and boolean");
    // The engine manages the frozen-metatable cache stored alongside this flag.
    // Directly changing bit 0 leaves bit 1 set and makes rawset fail at startup.
    g_engine.lua_setreadonly(L, 1, !!(uint32_t)state.value);
    return 0;
}
static int getconstant(void* L)
{
    void* proto = ref(function_arg(L, 1, 0), 0x18);
    int index = integer(L, 2, 1), count = *(int*)ptr(proto, VM_PROTO_CONSTANT_COUNT);
    Value value = index <= count ? ((Value*)ref(proto, VM_PROTO_CONSTANTS))[index - 1] : (Value){0};
    if (value.tag > 6) value = (Value){0};
    push(L, value);
    return 1;
}
static int getconstants(void* L)
{
    void* proto = ref(function_arg(L, 1, 0), 0x18);
    int count = *(int*)ptr(proto, VM_PROTO_CONSTANT_COUNT);
    if (count && !ref(proto, VM_PROTO_CONSTANTS)) return fail(L, "invalid prototype constant layout");
    void* table = g_engine.new_table(L, count, 0);
    push(L, object(table, 7));
    for (int i = 0; i < count; ++i)
    {
        Value value = ((Value*)ref(proto, VM_PROTO_CONSTANTS))[i];
        if (value.tag <= 6) put(L, table, number(i + 1), value);
    }
    return 1;
}
static int getfunctionhash(void* L)
{
    void* proto = ref(function_arg(L, 1, 0), 0x18);
    Sha384 hash; sha384_init(&hash);
    int constants = *(int*)ptr(proto, VM_PROTO_CONSTANT_COUNT);
    Value* values = ref(proto, VM_PROTO_CONSTANTS);
    for (int i = 0; i < constants; ++i)
    {
        Value value = values[i];
        if (value.tag == 6)
            sha384_update(&hash, ptr((void*)value.value, 0x18), *(uint32_t*)ptr((void*)value.value, 0x14));
        else if (value.tag == 3) sha384_update(&hash, &value.value, 8);
        else if (value.tag == 1) { uint8_t boolean = !!(uint32_t)value.value; sha384_update(&hash, &boolean, 1); }
    }
    unsigned count = *(unsigned*)ptr(proto, VM_PROTO_CODE_COUNT);
    uint32_t* instructions = ref(proto, 0x28);
    uint8_t inverse[256] = {0};
    for (unsigned op = 0; op < LOP_NEWCLASSMEMBER; ++op)
        inverse[*(uint8_t*)(g_engine.base +
#if defined(BASALT_ARM_744) && BASALT_ARM_744
            0x0084F959
#else
            0x0083FB29
#endif
            + (uint8_t)(op * 227u))] = op;
    for (unsigned i = 0; i < count;)
    {
        uint32_t word = instructions[i];
        uint8_t op = inverse[(uint8_t)word];
        word = (word & 0xffffff00u) | op;
        sha384_update(&hash, &word, 4);
        unsigned size = opcode_lengths[op];
        if (size == 2 && i + 1 < count) sha384_update(&hash, instructions + i + 1, 4);
        i += size;
    }
    char output[97]; sha384_finish(&hash, output);
    g_engine.lua_pushlstring(L, output, 96);
    return 1;
}
static int setconstant(void* L)
{
    void* proto = ref(function_arg(L, 1, 0), 0x18);
    int index = integer(L, 2, 1), count = *(int*)ptr(proto, VM_PROTO_CONSTANT_COUNT);
    if (index > count) return fail(L, "constant index out of range");
    Value value = arg(L, 3);
    if (g_engine.lua_gettop(L) < 3 || (value.tag != 0 && value.tag != 1 && value.tag != 3 && value.tag != 6))
        return fail(L, "constant must be nil, boolean, number, or string");
    Value* slot = (Value*)ref(proto, VM_PROTO_CONSTANTS) + index - 1;
    if (slot->tag > 6) return fail(L, "internal constant cannot be modified");
    *slot = value;
    barrier(L, proto, value);
    return 0;
}
static Value* upvalue(void* closure, int index)
{
    Value* slot = (Value*)ptr(closure, 0x20) + index - 1;
    return slot->tag == 16 ? ref((void*)slot->value, 8) : slot;
}
static int getupvalue(void* L)
{
    void* closure = function_arg(L, 1, 0);
    int index = integer(L, 2, 1);
    if (index > *ptr(closure, VM_LCLOSURE_NUPS)) return fail(L, "upvalue index out of range");
    push(L, *upvalue(closure, index));
    return 1;
}
static int getupvalues(void* L)
{
    void* closure = function_arg(L, 1, 0);
    unsigned count = *ptr(closure, VM_LCLOSURE_NUPS);
    void* table = g_engine.new_table(L, count, 0);
    push(L, object(table, 7));
    for (unsigned i = 1; i <= count; ++i) put(L, table, number(i), *upvalue(closure, i));
    return 1;
}
static int setupvalue(void* L)
{
    void* closure = function_arg(L, 1, 0);
    int index = integer(L, 2, 1);
    if (index > *ptr(closure, VM_LCLOSURE_NUPS)) return fail(L, "upvalue index out of range");
    if (g_engine.lua_gettop(L) < 3) return fail(L, "expected an upvalue replacement");
    Value value = arg(L, 3);
    Value* slot = (Value*)ptr(closure, 0x20) + index - 1;
    *upvalue(closure, index) = value;
    barrier(L, slot->tag == 16 ? (void*)slot->value : closure, value);
    return 0;
}
static int stack_values(void* L, int write)
{
    int level = integer(L, 1, 0);
    void* ci = frame(L, level);
    if (!ci) return 0;
    Value function =
#if BASALT_ARM_744
        ((Value*)ref(ci, 0))[-1];
#else
        *(Value*)ref(ci, 0x18);
#endif
    if (function.tag != 8 || *ptr((void*)function.value, 3)) return fail(L, "C stack frames are not supported");
    // CallInfo: +0 = frame base, +0x18 = frame limit. Registers of this frame end where
    // the active callee's function slot (one Value below its base) begins.
    Value* start = ref(ci, 0);
    Value* end = ref(ci,
#if BASALT_ARM_744
        0x18
#else
        8
#endif
    );
    if (level > 0)
    {
        Value* callee_base = ref(ptr(ci, 0x30), 0);
        if (callee_base && callee_base - 1 < end) end = callee_base - 1;
    }
    int count = (int)(end - start);
    if (count < 0 || count > 255) return fail(L, "invalid stack frame");
    if (!write && (g_engine.lua_gettop(L) == 1 || !arg(L, 2).tag))
    {
        void* table = g_engine.new_table(L, count, 0);
        push(L, object(table, 7));
        ci = frame(L, level); start = ref(ci, 0);
        for (int i = 0; i < count; ++i) put(L, table, number(i + 1), start[i]);
        return 1;
    }
    int index = integer(L, 2, 1);
    if (index > count) return fail(L, "stack index out of range");
    if (write)
    {
        if (g_engine.lua_gettop(L) < 3) return fail(L, "expected a stack replacement");
        if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
        start[index - 1] = arg(L, 3);
        return 0;
    }
    push(L, start[index - 1]);
    return 1;
}
static int getstack(void* L) { return stack_values(L, 0); }
static int setstack(void* L) { return stack_values(L, 1); }
static int debug_info(void* L)
{
    Value value = arg(L, 1);
    void* underlying = NULL;
    if (value.tag == 8)
    {
        void* closure = function_arg(L, 1, 1);
        if ((uintptr_t)closure != value.value)
        {
            push(L, value); // Keep the inactive proxy rooted while native info allocates.
            if (*ptr(L, VM_GCO_MARK) & 4) g_engine.thread_barrier(L, L, ptr(L, VM_L_GC_LIST));
            base(L)[0] = object(closure, 8);
            underlying = closure;
        }
    }
    int results = native_debug_info(L);
    if (underlying)
    {
        base(L)[0] = value;
        for (Value* slot = top(L) - results; slot < top(L); ++slot)
            if (slot->tag == 8 && slot->value == (uintptr_t)underlying) *slot = value;
    }
    return results;
}
static void register_one(void* L, void* env, const char* name, int (*function)(void*))
{
    g_engine.lua_pushcclosurek(L, function, name, 0, NULL);
    named(L, env, name, top(L)[-1]);
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
}
#if BASALT_DEV_BRIDGE && BASALT_ARM_744 && defined(__aarch64__)
static int rnet_start_api(void* L)
{
    int generation = rnet_start();
    if (!generation) return fail(L, "outgoing UDP hook unavailable for this engine");
    push(L, number(generation));
    return 1;
}
static int rnet_stop_api(void* L)
{
    rnet_stop(integer(L, 1, 1));
    return 0;
}
static int rnet_poll_api(void* L)
{
    int generation = integer(L, 1, 1);
    uint8_t* bytes = malloc(RNET_PACKET_MAX);
    if (!bytes) return fail(L, "packet buffer unavailable");
    size_t length = 0;
    int status = rnet_poll(generation, bytes, &length);
    if (status > 0) g_engine.lua_pushlstring(L, (const char*)bytes, length);
    else if (status < 0) g_engine.lua_pushboolean(L, 0);
    else push(L, (Value){0});
    free(bytes);
    return 1;
}
static int rnet_filter_api(void* L)
{
    Value data = arg(L, 1);
    if (data.tag != 6) return fail(L, "expected a byte string");
    size_t length = *(uint32_t*)(data.value + 0x14);
    if (length > 64) return fail(L, "prefix exceeds 64 bytes");
    if (!rnet_filter((const uint8_t*)(data.value + 0x18), length))
        return fail(L, "outgoing UDP hook unavailable for this engine");
    return 0;
}
static int rnet_send_api(void* L)
{
    Value data = arg(L, 1);
    if (data.tag != 6) return fail(L, "expected a byte string");
    size_t length = *(uint32_t*)(data.value + 0x14);
    if (!length || length > 65507) return fail(L, "packet length must be 1..65507 bytes");
    if (!rnet_send((const uint8_t*)(data.value + 0x18), length))
        return fail(L, "no active UDP destination or send failed");
    return 0;
}
#endif
void register_editor_ui_api(void* L, void* env)
{
    register_one(L, env, "_openDiscordInvite", open_discord_invite);
    register_one(L, env, "_editorInput", editor_input);
#if BASALT_DEV_BRIDGE
    register_one(L, env, "_serviceProtection", service_protection);
#endif
}
void register_vm_api(void* L, void* env)
{
#if BASALT_DEV_BRIDGE && BASALT_ARM_744 && defined(__aarch64__)
    register_one(L, env, "_rnetStart", rnet_start_api);
    register_one(L, env, "_rnetStop", rnet_stop_api);
    register_one(L, env, "_rnetPoll", rnet_poll_api);
    register_one(L, env, "_rnetFilter", rnet_filter_api);
    register_one(L, env, "_rnetSend", rnet_send_api);
#endif
#if BASALT_DEV_BRIDGE
    register_one(L, env, "_runtimeProtectionProbe", runtime_protection_probe_api);
#endif
    if (!native_debug_info)
    {
        void* original = original_environment(L);
        Value debug_key = object(g_engine.new_string(L, "debug", 5), 6);
        Value debug_table = *(Value*)g_engine.table_get(original, &debug_key);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
        LOGI("744 debug library: env=%p original=%p debug_tag=%u debug_ptr=%p",env,original,debug_table.tag,(void*)debug_table.value);
#endif
        if (debug_table.tag != 7) { fail(L, "native debug library unavailable"); return; }
        Value info_key = object(g_engine.new_string(L, "info", 4), 6);
        Value info = *(Value*)g_engine.table_get((void*)debug_table.value, &info_key);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
        if (info.tag==8 && info.value) {
            uint8_t* h=(uint8_t*)info.value;
            LOGI("744 debug.info: tag=%u header=%02x %02x %02x %02x %02x %02x %02x %02x fn=%p",
                info.tag,h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],ref((void*)info.value,0x18));
            LOGI("744 debug.info ptrs +10=%p +18=%p +20=%p +28=%p +30=%p",
                ref((void*)info.value,0x10),ref((void*)info.value,0x18),
                ref((void*)info.value,0x20),ref((void*)info.value,0x28),ref((void*)info.value,0x30));
        } else LOGI("744 debug.info: tag=%u value=%p",info.tag,(void*)info.value);
#endif
        if (info.tag != 8 || !*ptr((void*)info.value, 3) ||
            *ptr((void*)info.value, VM_CCLOSURE_NUPS))
        { fail(L, "unexpected native debug.info closure"); return; }
        native_debug_info = ref((void*)info.value, VM_CCLOSURE_FUNCTION);
    }
    register_one(L, env, "getfenv", getfenv);
    register_one(L, env, "_securityReady", security_ready);
    register_one(L, env, "_securitySeal", security_seal);
    register_one(L, env, "_securityRegister", security_register);
    register_one(L, env, "_securityRules", security_rule_list);
    register_one(L, env, "_securityNamecall", security_namecall);
    register_one(L, env, "setfenv", setfenv);
    register_one(L, env, "iscclosure", iscclosure);
    register_one(L, env, "islclosure", islclosure);
    register_one(L, env, "_closureUpvalueCount", closure_upvalue_count);
    register_one(L, env, "isexecutorclosure", isexecutorclosure);
    register_one(L, env, "fireproximityprompt", fire_proximity_prompt);
    register_one(L, env, "_fireClickDetector", fire_click_detector);
    register_one(L, env, "firetouchinterest", fire_touch_interest);
    register_one(L, env, "replicatesignal", replicate_signal);
#if BASALT_ARM_744
    register_one(L, env, "getsignalargumentsinfo", signal_arguments_info);
#endif
    register_one(L, env, "checkcaller", checkcaller);
    register_one(L, env, "clonefunction", clonefunction);
    register_one(L, env, "newcclosure", newcclosure);
    register_one(L, env, "isnewcclosure", isnewcclosure);
    register_one(L, env, "_unwrapCclosure", unwrap_cclosure);
    register_one(L, env, "hookfunction", hookfunction);
    register_one(L, env, "restorefunction", restorefunction);
    register_one(L, env, "isfunctionhooked", isfunctionhooked);
    register_one(L, env, "getgc", getgc);
    register_one(L, env, "_getThreads", get_threads);
    register_one(L, env, "_getUserdata", get_userdata);
    register_one(L, env, "gethwid", gethwid);
    register_one(L, env, "setfpscap", setfpscap);
    register_one(L, env, "getfpscap", getfpscap);
    register_one(L, env, "getreg", getreg);
    register_one(L, env, "getnamecallmethod", getnamecallmethod);
    register_one(L, env, "setnamecallmethod", setnamecallmethod);
    register_one(L, env, "setclipboard", setclipboard);
    register_one(L, env, "setrbxclipboard", setclipboard);
    register_one(L, env, "getclipboard", getclipboard);
    register_one(L, env, "_getDeviceBattery", device_battery);
    register_one(L, env, "_inputEvent", input_event);
    register_one(L, env, "_inputNativeCall", input_native_call);
    register_one(L, env, "isrbxactive", input_foreground);
    register_one(L, env, "getfunctionhash", getfunctionhash);
    register_one(L, env, "cloneref", cloneref);
    register_one(L, env, "compareinstances", compareinstances);
    register_one(L, env, "getscriptfromthread", getscriptfromthread);
    register_one(L, env, "_threadEnvironment", thread_environment);
    register_one(L, env, "getscriptbytecode", getscriptbytecode);
    register_one(L, env, "_getScriptBytecodeRaw", get_raw_script_bytecode);
    register_one(L, env, "getscriptclosure", getscriptclosure);
    register_one(L, env, "getscripthash", getscripthash);
    register_one(L, env, "_moduleLoaded", module_loaded);
    void* cache = g_engine.new_table(L, 0, 3);
    push(L, object(cache, 7));
    register_one(L, cache, "iscached", cache_iscached);
    register_one(L, cache, "invalidate", cache_invalidate);
    register_one(L, cache, "replace", cache_replace);
    named(L, env, "cache", object(cache, 7));
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
    register_one(L, env, "_callbackValue", callback_value);
    register_one(L, env, "_propertyDefault", property_default);
    register_one(L, env, "_getPropertyDescriptors", property_descriptors);
    register_one(L, env, "_instanceKey", instance_key);
    register_one(L, env, "_queueActor", queue_actor);
    register_one(L, env, "_pollActor", poll_actor_normal);
    register_one(L, env, "_cancelActor", poll_actor_cancel);
    register_one(L, env, "gethiddenproperty", read_property);
    register_one(L, env, "sethiddenproperty", write_property);
    register_one(L, env, "_installInstanceHttp", install_instance_http);
    register_one(L, env, "_disableInstanceDirectAccess", disable_instance_direct_access);
    register_one(L, env, "getthreadidentity", getthreadidentity);
    register_one(L, env, "setthreadidentity", setthreadidentity);
#ifdef BASALT_ENABLE_SECURITY_PROBE
    register_one(L, env, "_inspectInstanceLayout", inspect_instance_layout);
    register_one(L, env, "_probeObjectLayout", probe_object_layout);
    register_one(L, env, "_probeMemoryHex", probe_memory_hex);
    register_one(L, env, "_probeGlobalHex", probe_global_hex);
#endif
    register_one(L, env, "_snapshotConnections", snapshot_connections);
    register_one(L, env, "_setConnectionEnabled", set_connection_enabled);
    register_one(L, env, "_connectionEnabled", connection_enabled);
    register_one(L, env, "getrawmetatable", getrawmetatable);
    register_one(L, env, "setrawmetatable", setrawmetatable);
    register_one(L, env, "isreadonly", isreadonly);
    register_one(L, env, "setreadonly", setreadonly);
    void* debug = g_engine.new_table(L, 0, 16);
    push(L, object(debug, 7));
    register_one(L, debug, "info", debug_info);
    {
        void* original = original_environment(L);
        Value debug_key = object(g_engine.new_string(L, "debug", 5), 6);
        Value info_key = object(g_engine.new_string(L, "info", 4), 6);
        Value original_table = *(Value*)g_engine.table_get(original, &debug_key);
        if (original_table.tag == 7)
        {
            Value original_info = *(Value*)g_engine.table_get((void*)original_table.value, &info_key);
            Value wrapper = *(Value*)g_engine.table_get(debug, &info_key);
            if (original_info.tag == 8)
                put(L, registry_map(L, "__basalt_closure_origin_v1"), wrapper, original_info);
        }
    }
    register_one(L, debug, "getconstant", getconstant);
    register_one(L, debug, "getconstants", getconstants);
    register_one(L, debug, "setconstant", setconstant);
    register_one(L, debug, "getupvalue", getupvalue);
    register_one(L, debug, "getupvalues", getupvalues);
    register_one(L, debug, "setupvalue", setupvalue);
    register_one(L, debug, "getstack", getstack);
    register_one(L, debug, "setstack", setstack);
    register_one(L, debug, "getproto", getproto);
    register_one(L, debug, "getprotos", getprotos);
#if BASALT_ARM_744
    register_one(L, debug, "setname", debug_setname);
    register_one(L, debug, "setinfo", debug_setinfo);
#endif
    named(L, env, "_debugNative", object(debug, 7));
    *(Value**)ptr(L, VM_L_API_TOP) = top(L) - 1;
}
