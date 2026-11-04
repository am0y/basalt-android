#include "engine.h"
#include "vm_api.h"
#include "process_scope.h"
#include "process_memory.h"
#include "instance_scope.h"
#include "hook.h"
#include "protocol.h"
#include "embedded_bytecode.h"
#include "loader.h"
#include "compiler_bridge.h"
#include "workspace.h"
#include "http_bridge.h"
#include "build_config.h"
#include "legacy_diagnostics.h"
#include "runtime_protection.h"
#include "vm_layout.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#if BASALT_DEV_BRIDGE
__attribute__((used, retain)) static const char build_mode[] = "BASALT_DEVELOPMENT_BUILD_V1";
#else
__attribute__((used, retain)) static const char build_mode[] = "BASALT_RELEASE_NO_EXTERNAL_COMMANDS_V1";
#endif

#if BASALT_DEV_BRIDGE
#define CHANNEL_DIR "/sdcard/Android/data/com.roblox.client/files/basalt"
#define IN_DIR CHANNEL_DIR "/in"
#define OUT_DIR CHANNEL_DIR "/out"

struct pending_command {
    uint8_t* bytes;
    size_t length;
    char id[80];
};
#endif

struct protected_call_context {
    void* function;
    int nresults;
};

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
struct actor_job {
    uint32_t id;
    uintptr_t actor, shared[2];
    char* source;
    size_t length;
    uint64_t created;
    int state; // 0 free, 1 pending, 2 processing, 3 scheduled, 4 failed
    char error[160];
};
static struct actor_job actor_jobs[32];
static pthread_mutex_t actor_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned actor_pending, actor_serial;
static uint64_t actor_clock(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int executor_actor_enqueue(const uintptr_t shared[2], const char* source, size_t length, uint32_t* id)
{
    if (!shared[0] || !shared[1] || !source || !length || length > 16u * 1024u * 1024u) return 0;
    char* bytes = malloc(length);
    if (!bytes) return 0;
    memcpy(bytes, source, length);
    pthread_mutex_lock(&actor_mutex);
    struct actor_job* job = NULL;
    for (unsigned i = 0; i < 32; ++i) if (!actor_jobs[i].state) { job=&actor_jobs[i]; break; }
    int64_t owners = __atomic_load_n((int64_t*)(shared[1]+8), __ATOMIC_ACQUIRE);
    while (job && owners > 0 && owners < INT64_MAX &&
           !__atomic_compare_exchange_n((int64_t*)(shared[1]+8), &owners, owners+1, 0,
               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    if (!job || owners <= 0 || owners == INT64_MAX) {
        pthread_mutex_unlock(&actor_mutex); free(bytes); return 0;
    }
    *job = (struct actor_job){.id=++actor_serial,.actor=shared[0],
        .shared={shared[0],shared[1]},.source=bytes,.length=length,
        .created=actor_clock(),.state=1};
    if (!job->id) job->id=++actor_serial;
    *id=job->id;
    __atomic_fetch_add(&actor_pending, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&actor_mutex);
    return 1;
}

int executor_actor_poll(uint32_t id, int* status, char* error, size_t capacity, int cancel)
{
    pthread_mutex_lock(&actor_mutex);
    struct actor_job* job = NULL;
    for (unsigned i=0;i<32;++i) if (actor_jobs[i].state && actor_jobs[i].id==id) { job=&actor_jobs[i];break; }
    if (!job) { pthread_mutex_unlock(&actor_mutex); return 0; }
    if (job->state==1 && (cancel || actor_clock()-job->created > 20000)) {
        __atomic_fetch_sub(&actor_pending, 1, __ATOMIC_RELEASE);
        free(job->source);job->source=NULL;
        g_engine.release_shared(job->shared);job->shared[0]=job->shared[1]=0;
        job->state=4;
        snprintf(job->error,sizeof(job->error),"Actor VM did not resume before the timeout");
    }
    *status = job->state==3 ? 1 : job->state==4 ? -1 : 0;
    if (*status == -1 && capacity) snprintf(error,capacity,"%s",job->error);
    if (*status) memset(job,0,sizeof(*job));
    pthread_mutex_unlock(&actor_mutex);
    return 1;
}
#if BASALT_DEV_BRIDGE
static struct pending_command queue[16];
static size_t queue_length = 0;
static char current_request[80];
static int command_status;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static volatile int dispatching = 0;
static int editor_bootstrapping = 0;
static unsigned editor_poll;
static unsigned editor_retry_at;
static char editor_state_key;
static uintptr_t active_game_global;
struct observed_actor_vm { uintptr_t actor, global, main, model; uint64_t last_resume, capabilities; };
static struct observed_actor_vm observed_actor_vms[32];
static unsigned observed_actor_count;
typedef struct { uint64_t value; uint32_t extra, tag; } RuntimeValue;

static uintptr_t editor_attached_global; /* 743: registry unavailable, mark per-process */
static uintptr_t editor_attached_model;
static uintptr_t game_identity(void* L);
static int editor_vm_attached(void* L)
{
    uintptr_t global = *(uintptr_t*)((uint8_t*)L + VM_L_GLOBAL);
    return global && global == __atomic_load_n(&editor_attached_global, __ATOMIC_ACQUIRE) &&
        game_identity(L) == __atomic_load_n(&editor_attached_model, __ATOMIC_ACQUIRE);
}

static void mark_editor_vm(void* L)
{
    uintptr_t global = *(uintptr_t*)((uint8_t*)L + VM_L_GLOBAL);
    __atomic_store_n(&active_game_global,global,__ATOMIC_RELEASE);
    __atomic_store_n(&editor_attached_global,global,__ATOMIC_RELEASE);
    __atomic_store_n(&editor_attached_model,game_identity(L),__ATOMIC_RELEASE);
}

struct capability_entry { uint64_t value; };
// Proto.userdata stores an embedder-owned capability pointer. Keep these
// immutable values alive for all closures, including deferred descendants.
static struct capability_entry capability_entries[64];
static size_t capability_count;
static struct capability_entry trusted_capability_entries[64];
static size_t trusted_capability_count;
int executor_trusted_capability_pointer(const uint64_t* pointer)
{
    for (size_t i = 0; i < trusted_capability_count; ++i)
        if (pointer == &trusted_capability_entries[i].value) return 1;
    return 0;
}
int executor_capability_pointer(const uint64_t* pointer)
{
    if (executor_trusted_capability_pointer(pointer)) return 1;
    for (size_t i = 0; i < capability_count; ++i)
        if (pointer == &capability_entries[i].value) return 1;
    return 0;
}
static const uint64_t* capability_value(uint64_t value)
{
    for (size_t i = 0; i < capability_count; ++i)
        if (capability_entries[i].value == value)
            return &capability_entries[i].value;
    if (capability_count == 64)
        return NULL;
    capability_entries[capability_count].value = value;
    return &capability_entries[capability_count++].value;
}
static const uint64_t* trusted_capability_value(uint64_t value)
{
    for (size_t i = 0; i < trusted_capability_count; ++i)
        if (trusted_capability_entries[i].value == value) return &trusted_capability_entries[i].value;
    if (trusted_capability_count == 64) return NULL;
    trusted_capability_entries[trusted_capability_count].value = value;
    return &trusted_capability_entries[trusted_capability_count++].value;
}

#if BASALT_DEV_BRIDGE
static void report_for(const char* request, const char* text)
{
    pthread_mutex_lock(&log_mutex);
    LOGI("%s", text);
    FILE* out = fopen(OUT_DIR "/log.txt", "a");
    if (out)
    {
        fprintf(out, "%s\n", text);
        fclose(out);
    }
    if (request && request[0])
    {
        char path[512];
        snprintf(path, sizeof(path), OUT_DIR "/%s.log", request);
        out = fopen(path, "a");
        if (out)
        {
            fprintf(out, "%s\n", text);
            fclose(out);
        }
    }
    pthread_mutex_unlock(&log_mutex);
}

static void report(const char* text) { report_for(current_request, text); }

static void report_text(const char* request, const char* prefix, const char* text, size_t length)
{
    char line[768];
    size_t prefix_length = strlen(prefix);
    size_t used = prefix_length;
    memcpy(line, prefix, prefix_length);
    for (size_t i = 0; i < length; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || used == sizeof(line) - 1)
        {
            line[used] = 0;
            report_for(request, line);
            used = prefix_length;
            if (c == '\n') continue;
        }
        line[used++] = c == '\0' ? '?' : (char)c;
    }
    line[used] = 0;
    report_for(request, line);
}
#else
#define report(...) ((void)0)
#define report_vm_error(...) ((void)0)
#endif

static void dispatch_call(void* L, void* data)
{
    struct protected_call_context* ctx = data;
    report("entered luaD_callint");
    g_engine.luaD_callint(L, ctx->function, ctx->nresults, 0);
    report("returned from luaD_callint");
}

static int protected_call(void* L, int nargs, int nresults)
{
    uint8_t* state = L;
    uint8_t* top = *(uint8_t**)(state + VM_L_TOP);
    uint8_t* stack = *(uint8_t**)(state + 0x60); // Roblox 2.742.1059: lua_State::stack
    uint8_t* function = top - (size_t)(nargs + 1) * 16;
    struct protected_call_context ctx = {function, nresults};
    ptrdiff_t oldtop = function - stack;
    return g_engine.luaD_pcall(L, dispatch_call, &ctx, oldtop, 0);
}

#if BASALT_DEV_BRIDGE
static void probe_callback(void* L, void* context)
{
    (void)L;
    *(int*)context = 1;
}

static int probe_protected_call(void* L)
{
    uint8_t* state = L;
    uint8_t* top = *(uint8_t**)(state + VM_L_TOP);
    uint8_t* stack = *(uint8_t**)(state + VM_L_STACK);
    if (!top || !stack || top < stack || (top - stack) % 16 != 0)
    {
        report("protected-call probe: invalid Lua stack pointers");
        return 0;
    }
    int callback_ran = 0;
    int status = g_engine.luaD_pcall(L, probe_callback, &callback_ran, top - stack, 0);
    uint8_t* after_top = *(uint8_t**)(state + VM_L_TOP);
    uint8_t* after_stack = *(uint8_t**)(state + VM_L_STACK);
    char message[192];
    snprintf(message, sizeof(message), "protected-call probe status=%d callback=%d top=%p->%p stack=%p->%p",
             status, callback_ran, top, after_top, stack, after_stack);
    report(message);
    return status == 0 && callback_ran && after_stack && after_top >= after_stack &&
        (size_t)(after_top - after_stack) == (size_t)(top - stack);
}

static int native_noop(void* L)
{
    (void)L;
    report("native closure invoked");
    return 0;
}

static void probe_native_call(void* L)
{
    uint8_t* original_top = *(uint8_t**)((uint8_t*)L + VM_L_TOP);
    int before = g_engine.lua_gettop(L);
    report("creating native closure");
    g_engine.lua_pushcclosurek(L, native_noop, "basalt_call_probe", 0, NULL);
    int after = g_engine.lua_gettop(L);
    char message[100];
    snprintf(message, sizeof(message), "native closure stack %d->%d", before, after);
    report(message);
    if (after != before + 1)
        return;

    report("calling native closure through protected VM bridge");
    int result = protected_call(L, 0, 0);
    snprintf(message, sizeof(message), "native call returned status=%d top=%d", result, g_engine.lua_gettop(L));
    report(message);
    if (result != 0)
        *(uint8_t**)((uint8_t*)L + VM_L_TOP) = original_top;
}
#endif

// Read through the kernel to avoid faults on stale VM pointers. Some unrooted
// Android devices deny /proc/self/mem but allow process_vm_readv on themselves.
static int read_vm(uintptr_t address, void* out, size_t length)
{
    return process_memory_read(address, out, length);
}

static void restore_stack_top(uint8_t* state, size_t original_stack_offset)
{
    uint8_t* stack = *(uint8_t**)(state + VM_L_STACK);
    *(uint8_t**)(state + VM_L_TOP) = stack + original_stack_offset;
}

#if BASALT_DEV_BRIDGE
static void report_vm_error_for(void* L, const char* phase, const char* request)
{
    uintptr_t top = 0, error = 0;
    uint32_t tag = 0, length = 0;
    char message[600];
    if (!read_vm((uintptr_t)L + VM_L_TOP, &top, sizeof(top)) || top < 16 ||
        !read_vm(top - 4, &tag, sizeof(tag)))
    {
        report_for(request, "error value unavailable");
        return;
    }
    // Roblox 2.742.1059: LUA_TSTRING=6, TString.len=0x14, data=0x18.
    if (tag == 6 && read_vm(top - 16, &error, sizeof(error)) &&
        read_vm(error + 0x14, &length, sizeof(length)))
    {
        char text[2048] = {0};
        size_t size = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
        if (read_vm(error + 0x18, text, size))
        {
            snprintf(message, sizeof(message), "%s error: ", phase);
            report_text(request, message, text, size);
            if (length > size) report_for(request, "error detail truncated at 2047 bytes");
            return;
        }
    }
    snprintf(message, sizeof(message), "%s error: non-string or unreadable value (tag=%u)", phase, tag);
    report_for(request, message);
}

static void report_vm_error(void* L, const char* phase) { report_vm_error_for(L, phase, current_request); }

static void inspect_loaded_function(uint8_t* top)
{
    uintptr_t closure = 0, proto = 0, code = 0, codeentry = 0;
    uint8_t is_c = 0;
    uint32_t instructions[4] = {0};
    int sizecode = 0;
    char message[260];

    if (!read_vm((uintptr_t)(top - 16), &closure, sizeof(closure)) ||
        !read_vm(closure + 3, &is_c, sizeof(is_c)) ||
        !read_vm(closure + 0x18, &proto, sizeof(proto)) ||
        !read_vm(proto + 0x28, &code, sizeof(code)) ||
        !read_vm(proto + 0x78, &codeentry, sizeof(codeentry)) ||
        !read_vm(proto + 0xa8, &sizecode, sizeof(sizecode)) ||
        sizecode <= 0 || sizecode > 1000000 ||
        !read_vm(code, instructions, (sizecode < 4 ? sizecode : 4) * sizeof(uint32_t)))
    {
        report("inspect: cannot read closure/proto/code; VM layout or /proc/self/mem access differs");
        return;
    }
    snprintf(message, sizeof(message),
             "inspect LIVE: cl=%p isC=%u proto=%p code=%p entry=%p ncode=%d",
             (void*)closure, is_c, (void*)proto, (void*)code, (void*)codeentry, sizecode);
    report(message);
    snprintf(message, sizeof(message),
             "inspect LIVE words: %08x %08x %08x %08x (bytes: %02x %02x %02x %02x)",
             instructions[0], instructions[1], instructions[2], instructions[3],
             instructions[0] & 0xff, instructions[1] & 0xff,
             instructions[2] & 0xff, instructions[3] & 0xff);
    report(message);
}

static void compare_loaded_function(void* L, uint8_t* top)
{
    uintptr_t global = 0, thread = 0, proto = 0;
    uint8_t proto_header[8] = {0};
    uint32_t instruction = 0;
    int original_top = g_engine.lua_gettop(L);

    if (!read_vm((uintptr_t)L + VM_L_GLOBAL, &global, sizeof(global)) ||
        !read_vm(global + 0x60, &thread, sizeof(thread)))
    {
        report("compare: cannot read VM global/mainthread");
        return;
    }

    // The game's coroutine argument at index 1 is on the current C frame.
    uint8_t* base = *(uint8_t**)((uint8_t*)L + VM_L_BASE);
    uint32_t argtag = 0;
    read_vm((uintptr_t)base + 12, &argtag, sizeof(argtag));
    char message[256];
    snprintf(message, sizeof(message), "compare: L=%p global=%p main=%p base=%p arg1tag=%u",
             L, (void*)global, (void*)thread, base, argtag);
    report(message);

    if (argtag != 10)
        return;
    uintptr_t co = 0;
    if (!read_vm((uintptr_t)base, &co, sizeof(co)))
        return;

    // Read the coroutine's first function slot, if still suspended and intact.
    uint8_t costatus = 0;
    uintptr_t co_stack = 0;
    uintptr_t co_top = 0;
    if (!read_vm(co + 3, &costatus, 1) ||
        !read_vm(co + 0x30, &co_stack, sizeof(co_stack)) ||
        !read_vm(co + 0x20, &co_top, sizeof(co_top)))
        return;
    snprintf(message, sizeof(message), "compare: co=%p status=%u stack=%p top=%p",
             (void*)co, costatus, (void*)co_stack, (void*)co_top);
    report(message);

    if (!co_stack || co_top < co_stack + 16 || co_top > co_stack + 128 * 1024)
        return;

    for (size_t slot = 0; slot < 3; ++slot)
    {
        uintptr_t address = co_stack + slot * 16;
        uint32_t tag = 0;
        uintptr_t value = 0;
        if (!read_vm(address, &value, 8) || !read_vm(address + 12, &tag, 4))
            break;
        snprintf(message, sizeof(message), "compare: co stack[%zu] tag=%u value=%p",
                 slot, tag, (void*)value);
        report(message);
        if (tag != 8)
            continue;
        if (!read_vm(value + 0x18, &proto, 8) ||
            !read_vm(proto, proto_header, sizeof(proto_header)))
            continue;
        snprintf(message, sizeof(message), "compare: native proto header=%02x %02x %02x %02x %02x %02x %02x %02x",
                 proto_header[0], proto_header[1], proto_header[2], proto_header[3],
                 proto_header[4], proto_header[5], proto_header[6], proto_header[7]);
        report(message);
        break;
    }

    (void)top;
    (void)instruction;
    if (g_engine.lua_gettop(L) != original_top)
        report("compare: altered stack unexpectedly");
}

static void inspect_context(void* L)
{
    char message[240];
    report("context scan: lua_State fields 0x78..0xd0");
    for (uintptr_t offset = 0x78; offset <= 0xd0; offset += 8)
    {
        uintptr_t value = 0;
        uint8_t bytes[16] = {0};
        read_vm((uintptr_t)L + offset, &value, sizeof(value));
        int readable = value && read_vm(value, bytes, sizeof(bytes));
        snprintf(message, sizeof(message),
                 "context +0x%02lx=%p%s %02x %02x %02x %02x %02x %02x %02x %02x",
                 (unsigned long)offset, (void*)value, readable ? " ->" : "",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
        report(message);
    }
}
#endif

static int inherit_proto_capabilities(uintptr_t proto, const uint64_t* capabilities, unsigned depth)
{
    uint8_t tag = 0;
    uintptr_t children = 0;
    int count = 0;
    if (depth > 200 || !proto || !read_vm(proto + VM_GCO_TYPE, &tag, 1) || tag != 15 ||
        !read_vm(proto + VM_PROTO_CHILDREN, &children, sizeof(children)) ||
        !read_vm(proto + VM_PROTO_CHILD_COUNT, &count, sizeof(count)) || count < 0 || count > 32768)
        return 0;
    for (int i = 0; i < count; ++i)
    {
        uintptr_t child = 0;
        if (!read_vm(children + (size_t)i * 8, &child, 8) ||
            !inherit_proto_capabilities(child, capabilities, depth + 1))
            return 0;
    }
    memcpy((void*)(proto + VM_PROTO_CAPABILITIES), &capabilities, sizeof(capabilities));
    return 1;
}

static int prepare_loaded_closure(void* L, const uint64_t* capabilities)
{
    uintptr_t top = *(uintptr_t*)((uint8_t*)L + VM_L_TOP);
    uintptr_t closure = 0, proto = 0;
    uint32_t tag = 0;
    uint8_t is_c = 0;
    return capabilities && read_vm(top - 4, &tag, 4) && tag == 8 &&
        read_vm(top - 16, &closure, 8) && read_vm(closure + 3, &is_c, 1) && !is_c &&
        read_vm(closure + 0x18, &proto, 8) && inherit_proto_capabilities(proto, capabilities, 0);
}
int executor_prepare_closure(void* L)
{
    uint64_t effective = g_engine.effective_capabilities(L);
    pthread_mutex_lock(&queue_mutex);
    const uint64_t* capabilities = capability_value(effective);
    pthread_mutex_unlock(&queue_mutex);
    return capabilities && prepare_loaded_closure(L, capabilities);
}

#if BASALT_DEV_BRIDGE
static const RuntimeValue* callback_upvalue(void* L);
static RuntimeValue* value_base(void* L);
static int scheduled_done_744(void* L)
{
    RuntimeValue* arguments = *(RuntimeValue**)((uint8_t*)L + VM_L_BASE);
    if (g_engine.lua_gettop(L) < 2 || arguments[0].tag != 6 || !arguments[0].value) return 0;
    uint32_t size = 0;
    if (!read_vm(arguments[0].value + 0x14, &size, 4) || !size || size >= 80) return 0;
    char token[80];
    if (!read_vm(arguments[0].value + 0x18, token, size)) return 0;
    token[size] = 0;
    int success = arguments[1].tag == 1 && (uint32_t)arguments[1].value != 0;
    if (!success && g_engine.lua_gettop(L) >= 3 && arguments[2].tag == 6 && arguments[2].value) {
        uint32_t length = 0;
        if (read_vm(arguments[2].value + 0x14, &length, 4) && length < 16384)
            report_text(token, "script error: ", (const char*)(arguments[2].value + 0x18), length);
    }
    report_for(token, success ? "script completed status=0" : "script completed status=2");
    return 0;
}
static int scheduled_done(void* L)
{
#if BASALT_ARM_744
    const RuntimeValue* token = callback_upvalue(L);
    uint32_t length=0;
    if (!token || token->tag!=6 || !token->value ||
        !read_vm(token->value+0x14,&length,4) || !length || length>=sizeof(current_request)) return 0;
    char request[80];
    if (!read_vm(token->value+0x18,request,length)) return 0;
    request[length]=0;
    RuntimeValue* args=value_base(L);
    int success=g_engine.lua_gettop(L)>0 && args[0].tag==1 && (uint32_t)args[0].value!=0;
    if (!success && g_engine.lua_gettop(L)>1 && args[1].tag==6 && args[1].value) {
        uint32_t size=0;
        if(read_vm(args[1].value+0x14,&size,4) && size<16384)
            report_text(request,"script error: ",(const char*)(args[1].value+0x18),size);
    }
    report_for(request,success ? "script completed status=0" : "script completed status=2");
    return 0;
#else
    // The C closure's first upvalue stores the short ASCII command filename.
    uintptr_t ci = 0, slot = 0, closure = 0, string = 0;
    uint32_t length = 0, tag = 0;
    if (!read_vm((uintptr_t)L + 8, &ci, 8) ||
        !read_vm(ci + 0x18, &slot, 8) ||
        !read_vm(slot, &closure, 8) ||
        !read_vm(closure + 0x30, &string, 8) ||
        !read_vm(closure + 0x3c, &tag, 4) || tag != 6 ||
        !read_vm(string + 0x14, &length, 4) ||
        length == 0 || length >= sizeof(current_request))
        return 0;
    char request[80];
    memcpy(request, (const void*)(string + 0x18), length);
    request[length] = 0;
    uint8_t* base = *(uint8_t**)((uint8_t*)L + VM_L_BASE);
    int success = *(uint32_t*)(base + 12) == 1 && *(int*)base != 0;
    if (!success)
        report_vm_error_for(L, "script", request);
    report_for(request, success ? "script completed status=0" : "script completed status=2");
    return 0;
#endif
}

struct schedule_context {
    const uint8_t* bytes;
    size_t length;
    const uint64_t* capabilities;
    void* child;
};
#endif


// 743: raw, VM-op-free table hash scan (node@+0x28, stride 32, metatable@+0x50).
static int raw_string_equal(uintptr_t tstring, const char* name, size_t length)
{
    char buffer[32];
    if (length >= sizeof(buffer)) return 0;
    uint32_t len = 0;
    if (!read_vm(tstring + 0x14, &len, 4) || len != length) return 0;   // TString len @+0x14 (tentative)
    if (!read_vm(tstring + 0x18, buffer, length)) return 0;             // data @+0x18 (tentative)
    return !memcmp(buffer, name, length);
}
static RuntimeValue raw_table_get(uintptr_t table, const char* name)
{
    size_t length = strlen(name);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    static unsigned table_probes;
    unsigned probe = !strcmp(name,"game") ? __atomic_fetch_add(&table_probes,1,__ATOMIC_RELAXED) : UINT_MAX;
#endif
    for (unsigned depth = 0; table && depth < 6; ++depth)
    {
        uintptr_t node = 0;
        unsigned max_nodes = 1024;
#if BASALT_ARM_744
        uint8_t log_size = 0;
        if (read_vm(table + 6, &log_size, 1) && log_size <= 10)
            max_nodes = 1u << log_size;
#endif
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
        if (probe < 4) {
            uint8_t header[48] = {0};
            read_vm(table,header,sizeof(header));
            LOGI("744 table depth=%u table=%p header=%02x %02x %02x %02x %02x %02x %02x %02x pointers=%llx %llx %llx %llx",
                depth,(void*)table,header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7],
                (unsigned long long)*(uint64_t*)(header+8),(unsigned long long)*(uint64_t*)(header+16),
                (unsigned long long)*(uint64_t*)(header+24),(unsigned long long)*(uint64_t*)(header+32));
        }
#endif
        if (!read_vm(table + VM_TABLE_NODE, &node, 8) || !node) break;
        for (unsigned i = 0; i < max_nodes; ++i)
        {
            RuntimeValue value = {0}, key = {0};
            if (!read_vm(node + i * 32 + 16, &key, sizeof(key))) break;      // LuaNode: val(16) then key(16)
            if (!read_vm(node + i * 32, &value, sizeof(value))) break;
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
            if (probe < 2 && depth < 2 && i < 8)
                LOGI("744 node index=%u node=%p keytag=%u key=%p valtag=%u val=%p",i,(void*)node,key.tag,(void*)key.value,value.tag,(void*)value.value);
            if (probe < 1 && depth < 2 && i < 64 && (key.tag & 15u) == 6 && key.value) {
                uint32_t len = 0;
                char text[33] = {0};
                if (read_vm(key.value + 0x14, &len, sizeof(len)) && len > 0 && len <= 32 &&
                    read_vm(key.value + 0x18, text, len))
                    LOGI("744 node string depth=%u index=%u len=%u key=%s valtag=%u",depth,i,len,text,value.tag);
            }
#endif
            if ((key.tag & 15u) == 6 && key.value && raw_string_equal(key.value, name, length))
                return value;
#if !BASALT_ARM_744
            if (key.tag == 0 && value.tag == 0 && i > 8) break;              // old empty-tail heuristic
#endif
        }
        uintptr_t metatable = 0;
        if (!read_vm(table + VM_TABLE_METATABLE, &metatable, 8) || !metatable) break;
#if BASALT_ARM_744
        /* The script environment inherits from its metatable's __index table.
           Scanning the metatable as if it were the environment misses game. */
        uint8_t mt_log_size = 0;
        uintptr_t mt_nodes = 0;
        if (!read_vm(metatable + VM_GCO_TYPE, &mt_log_size, 1) || mt_log_size != 7 ||
            !read_vm(metatable + 6, &mt_log_size, 1) || mt_log_size > 10 ||
            !read_vm(metatable + VM_TABLE_NODE, &mt_nodes, 8) || !mt_nodes) break;
        uintptr_t inherited = 0;
        for (unsigned i = 0; i < (1u << mt_log_size); ++i) {
            RuntimeValue key = {0}, value = {0};
            if (!read_vm(mt_nodes + i * 32 + 16, &key, sizeof(key)) ||
                !read_vm(mt_nodes + i * 32, &value, sizeof(value))) break;
            if ((key.tag & 15u) == 6 && value.tag == 7 && key.value &&
                raw_string_equal(key.value, "__index", 7)) {
                inherited = value.value;
                break;
            }
        }
        table = inherited;
#else
        table = metatable;
#endif
    }
    return (RuntimeValue){0};
}
static RuntimeValue raw_active_lookup(void* L, const char* name)
{
    uintptr_t environment = *(uintptr_t*)((uint8_t*)L + VM_L_ENV);
    RuntimeValue value = raw_table_get(environment, name);
    if (value.tag) return value;
    uintptr_t ci = *(uintptr_t*)((uint8_t*)L + VM_L_CI);
    uintptr_t first = *(uintptr_t*)((uint8_t*)L + VM_L_BASE_CI);
    for (unsigned depth = 0; depth < 24 && ci >= first && ci - first < 0x1000; ++depth, ci -= 0x30)
    {
        uintptr_t slot = 0; RuntimeValue function = {0}; uintptr_t closure_env = 0;
        if (!read_vm(ci + 0x18, &slot, 8) || !slot) continue;
        if (!read_vm(slot, &function, sizeof(function)) || function.tag != 8 || !function.value) continue;
        if (!read_vm(function.value + 0x10, &closure_env, 8) || !closure_env) continue;
        value = raw_table_get(closure_env, name);
        if (value.tag) return value;
    }
    return (RuntimeValue){0};
}

static void boot_trace(const char* text)
{
#if BASALT_DEV_BRIDGE
    FILE* out = fopen("/sdcard/Android/data/com.roblox.client/files/basalt-boot.txt", "a");
    if (out) { fprintf(out, "%s\n", text); fclose(out); }
#else
    (void)text;
#endif
}

static RuntimeValue environment_lookup(void* L, void* environment, const char* name);
static RuntimeValue active_script_lookup(void* L, const char* name);
static uintptr_t game_identity(void* L);

static RuntimeValue* value_top(void* L) { return *(RuntimeValue**)((uint8_t*)L + VM_L_TOP); }
static RuntimeValue* value_base(void* L) { return *(RuntimeValue**)((uint8_t*)L + VM_L_BASE); }
static void push_value(void* L, RuntimeValue value)
{
    if (!g_engine.check_stack(L, 1)) return;
    if (*((uint8_t*)L + VM_L_GC_FLAGS) & 4) g_engine.thread_barrier(L, L, (uint8_t*)L + VM_L_GC_LIST);
    RuntimeValue* top = value_top(L);
    *top = value;
    *(RuntimeValue**)((uint8_t*)L + VM_L_TOP) = top + 1;
}

static void table_write(void* L, void* table, const char* name, RuntimeValue value)
{
    RuntimeValue key = {.value=(uintptr_t)g_engine.new_string(L, name, strlen(name)), .tag=6};
    RuntimeValue* slot = g_engine.table_set(L, table, &key);
    *slot = value;
    if (value.tag >= 6 && (*((uint8_t*)table + VM_L_GC_FLAGS) & 4) &&
        (*((uint8_t*)value.value + VM_L_GC_FLAGS) & 3))
        g_engine.table_barrier(L, table, (void*)value.value);
}

struct new_thread_context { void* child; };
static void create_thread_from_main(void* main, void* data)
{
    ((struct new_thread_context*)data)->child = g_engine.lua_newthread(main);
}

static void* new_executor_thread(void* parent)
{
    uintptr_t global = *(uintptr_t*)((uint8_t*)parent + VM_L_GLOBAL);
    uintptr_t main = 0, owner = 0;
    if (!read_vm(global + VM_GLOBAL_MAINTHREAD, &main, sizeof(main)) || !main ||
        !read_vm(main + VM_L_GLOBAL, &owner, sizeof(owner)) || owner != global)
    { report("executor main VM thread is unavailable"); return NULL; }
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    { char line[160]; snprintf(line,sizeof(line),"744 child root parent=%p global=%p main=%p owner=%p",parent,(void*)global,(void*)main,(void*)owner); report(line); }
#endif
    if (!g_engine.check_stack(parent, 1))
    { report("executor parent stack limit"); return NULL; }
#if BASALT_ARM_744
    /* The old cross-main-thread root transfer is incompatible with the new
       call frame layout. Use the current game script's VM as the child owner. */
    void* child = g_engine.lua_newthread(parent);
    if (!vm_copy_thread_capabilities(child, parent)) return NULL;
    return child;
#endif
    if (main == (uintptr_t)parent) return g_engine.lua_newthread(parent);

    // A game LocalScript's child inherits its script-lifetime context. Starting
    // from the VM main thread avoids losing GUI connections when that script
    // is destroyed. Keep the new child rooted while moving it to our caller.
    size_t offset = *(uintptr_t*)(main + VM_L_TOP) - *(uintptr_t*)(main + VM_L_STACK);
    struct new_thread_context context = {NULL};
    int status = g_engine.luaD_pcall((void*)main, create_thread_from_main, &context, offset, 0);
    if (status)
    {
        report_vm_error((void*)main, "executor thread creation");
        restore_stack_top((uint8_t*)main, offset);
        return NULL;
    }
    RuntimeValue value = value_top((void*)main)[-1];
    if (value.tag != 10 || value.value != (uintptr_t)context.child)
    {
        restore_stack_top((uint8_t*)main, offset);
        report("executor thread root is invalid");
        return NULL;
    }
    if (!vm_copy_thread_capabilities(context.child, parent))
    {
        restore_stack_top((uint8_t*)main, offset);
        report("executor capability inheritance failed");
        return NULL;
    }
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    { uintptr_t child_global = 0; read_vm((uintptr_t)context.child+VM_L_GLOBAL,&child_global,8);
      char line[160]; snprintf(line,sizeof(line),"744 child=%p global=%p expected=%p",context.child,(void*)child_global,(void*)global); report(line); }
#endif
    push_value(parent, value);
    restore_stack_top((uint8_t*)main, offset);
    return context.child;
}

static void* make_environment(void* L, void* game_thread)
{
    uintptr_t global = *(uintptr_t*)((uint8_t*)L + VM_L_GLOBAL);
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    { uintptr_t parent_global=0, intern=0; read_vm((uintptr_t)game_thread+VM_L_GLOBAL,&parent_global,8); read_vm(global,&intern,8);
      char line[190]; snprintf(line,sizeof(line),"744 environment start child=%p parent=%p global=%p parentglobal=%p global0=%p",L,game_thread,(void*)global,(void*)parent_global,(void*)intern); report(line); }
#endif
    void* registry = NULL; /* 743 registry pending */
    const char* registry_key = "__basalt_shared_v3";
    RuntimeValue key = {.value=(uintptr_t)g_engine.new_string(L, registry_key, strlen(registry_key)), .tag=6};
    const RuntimeValue* found = registry ? g_engine.table_get(registry, &key) : NULL;
    void* shared = (found && found->tag == 7) ? (void*)found->value : NULL;
    uintptr_t main = *(uintptr_t*)(global + VM_GLOBAL_MAINTHREAD);
    void* original = main ? *(void**)(main + VM_L_ENV) : *(void**)((uint8_t*)L + VM_L_ENV);
    // Script sandboxes hold the game's shared _G/shared tables; the main
    // builtin environment may omit them. Preserve their identities for getrenv.
    RuntimeValue runtime_globals = active_script_lookup(game_thread, "_G");
    RuntimeValue runtime_shared = active_script_lookup(game_thread, "shared");
    if (runtime_globals.tag == 7) table_write(L, original, "_G", runtime_globals);
    if (runtime_shared.tag == 7) table_write(L, original, "shared", runtime_shared);
    if (!shared)
    {
        shared = g_engine.new_table(L, 0, 16);
        void* mt = g_engine.new_table(L, 0, 1);
        table_write(L, mt, "__index", (RuntimeValue){.value=(uintptr_t)original, .tag=7});
        memcpy((uint8_t*)shared + VM_TABLE_METATABLE, &mt, 8);
        table_write(L, shared, "_G", (RuntimeValue){.value=(uintptr_t)shared, .tag=7});
        table_write(L, shared, "shared", (RuntimeValue){.value=(uintptr_t)g_engine.new_table(L, 0, 0), .tag=7});
        RuntimeValue game = active_script_lookup(game_thread, "game");
        if (game.tag == 9) table_write(L, shared, "game", game);
#if BASALT_DEV_BRIDGE
        RuntimeValue debug = environment_lookup(L, original, "debug");
        LOGI("742 executor environment: main=%p inherited=%p debug_tag=%u game_tag=%u",
             (void*)main, original, debug.tag, game.tag);
#endif
        if (registry) table_write(L, registry, registry_key, (RuntimeValue){.value=(uintptr_t)shared, .tag=7});
    }
    void* env = g_engine.new_table(L, 0, 16);
    void* mt = g_engine.new_table(L, 0, 1);
    table_write(L, mt, "__index", (RuntimeValue){.value=(uintptr_t)shared, .tag=7});
    memcpy((uint8_t*)env + VM_TABLE_METATABLE, &mt, 8);
    *((uint8_t*)env + 7) = 0; // Clear safeenv so GETIMPORT resolves through __index.
    RuntimeValue debug = environment_lookup(L, original, "debug");
    if (debug.tag == 7) table_write(L, env, "debug", debug);
    *(void**)((uint8_t*)L + VM_L_ENV) = env;
    return shared;
}

static const RuntimeValue* callback_upvalue(void* L)
{
#if BASALT_ARM_744
    uintptr_t closure = value_base(L)[-1].value;
    return (const RuntimeValue*)(closure + 0x30);
#else
    uintptr_t ci = *(uintptr_t*)((uint8_t*)L + VM_L_CI);
    uintptr_t slot = *(uintptr_t*)(ci + 0x18);
    uintptr_t closure = *(uintptr_t*)slot;
    return (const RuntimeValue*)(closure + 0x30);
#endif
}

#if BASALT_DEV_BRIDGE
static int script_emit(void* L)
{
    const RuntimeValue* id = callback_upvalue(L);
    RuntimeValue* base = value_base(L);
    if (id->tag != 6 || g_engine.lua_gettop(L) < 1 || base->tag != 6) return 0;
    uint32_t idsize = *(uint32_t*)(id->value + 0x14);
    uint32_t size = *(uint32_t*)(base->value + 0x14);
    if (!idsize || idsize >= 80) return 0;
    char request[80];
    memcpy(request, (void*)(id->value + 0x18), idsize); request[idsize] = 0;
    size_t amount = size < 16384 ? size : 16384;
    report_text(request, "output: ", (const char*)(base->value + 0x18), amount);
    if (size > amount) report_for(request, "output: [truncated at 16384 bytes]");
    return 0;
}
#endif

static int loadstring_failure(void* L, const char* message)
{
    push_value(L, (RuntimeValue){0});
    g_engine.lua_pushstring(L, message);
    return 2;
}

static int script_loadstring(void* L)
{
    int argc = g_engine.lua_gettop(L);
    RuntimeValue* base = value_base(L);
    if (!argc || base[0].tag != 6) return loadstring_failure(L, "loadstring expects a source string");
    uint32_t length = *(uint32_t*)(base[0].value + 0x14);
    if (length > 8u * 1024u * 1024u) return loadstring_failure(L, "source exceeds 8 MiB");
    const char* source = (const char*)(base[0].value + 0x18);
    char name[100] = "=loadstring";
    if (argc > 1 && base[1].tag == 6)
    {
        uint32_t n = *(uint32_t*)(base[1].value + 0x14);
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, (void*)(base[1].value + 0x18), n); name[n] = 0;
    }
    size_t size = 0;
    char* bytes = compile_encoded(source, length, &size);
    if (!bytes) return loadstring_failure(L, "compiler allocation failed");
    if (!size || !bytes[0])
    {
        char error[512] = "compiler returned empty bytecode";
        if (size > 1) { size_t n = size - 1 < sizeof(error)-1 ? size-1 : sizeof(error)-1; memcpy(error, bytes+1,n); error[n]=0; }
        free(bytes);
        return loadstring_failure(L, error);
    }
    uint64_t caps = g_engine.effective_capabilities(L);
    pthread_mutex_lock(&queue_mutex);
    const uint64_t* capabilities = capability_value(caps);
    pthread_mutex_unlock(&queue_mutex);
    if (!capabilities) { free(bytes); return loadstring_failure(L, "capability storage exhausted"); }
    const RuntimeValue* up = callback_upvalue(L);
    void* oldenv = *(void**)((uint8_t*)L + VM_L_ENV);
    if (up->tag == 7) *(void**)((uint8_t*)L + VM_L_ENV) = (void*)up->value;
    int status = bytecode_load(L, name, (uint8_t*)bytes, size, capabilities);
#if BASALT_ARM_744
    if (!status && !prepare_loaded_closure(L, capabilities)) status = 1;
#endif
    *(void**)((uint8_t*)L + VM_L_ENV) = oldenv;
    free(bytes);
    if (!status) return 1;
    RuntimeValue error = value_top(L)[-1];
    push_value(L, error);
    value_top(L)[-2] = (RuntimeValue){0};
    return 2;
}

#if BASALT_DEV_BRIDGE && BASALT_SCHEDULER_ENABLED
static void schedule_on_child(void* parent, void* data)
{
    struct schedule_context* ctx = data;
#if BASALT_ARM_744 && !BASALT_ARM_744_FULL
    /* Bootstrap on the current live game VM; task.defer creates a real
       yielding coroutine. The old cross-main-thread child and manual proto
       constructor use layouts from another build and are not usable here. */
static const char runner_source[] =
        "return function(f,done,token) task.defer(function() "
        "local ok,err=xpcall(f,function(e) return tostring(e) end); "
        "done(token,ok,err) end) end";
    size_t length = 0;
    char* runner = compile_encoded(runner_source, sizeof(runner_source) - 1, &length);
    if (!runner || !length || !runner[0]) {
        free(runner); report("744 runner compile failed"); command_status = 1; return;
    }
    int status = engine_load_submitted(parent, "=basalt.runner", runner, length, 0);
    free(runner);
    uint64_t mask = g_engine.effective_capabilities(parent);
    const uint64_t* capabilities = capability_value(mask);
    if (!status && capabilities) {
        int prepared = prepare_loaded_closure(parent, capabilities);
        char line[120]; snprintf(line,sizeof(line),"744 runner capability mask=%llx proto=%d",(unsigned long long)mask,prepared); report(line);
    }
    if (!status) status = protected_call(parent, 0, 1);
    if (status) { report_vm_error(parent, "744 runner"); command_status = status; return; }
    char chunkname[100];
    snprintf(chunkname, sizeof(chunkname), "@%s", current_request);
    status = engine_load_submitted(parent, chunkname, (const char*)ctx->bytes, ctx->length, 0);
    if (status) { report_vm_error(parent, "744 load"); command_status = status; return; }
    if (capabilities) {
        int prepared = prepare_loaded_closure(parent, capabilities);
        char line[100]; snprintf(line,sizeof(line),"744 source proto=%d",prepared); report(line);
    }
    g_engine.lua_pushcclosurek(parent, scheduled_done_744, "basalt_done", 0, NULL);
    g_engine.lua_pushstring(parent, current_request);
    status = protected_call(parent, 3, 0);
    if (status) { report_vm_error(parent, "744 dispatch"); command_status = status; return; }
    report("script scheduled");
    command_status = 0;
    return;
#else
    void* L = new_executor_thread(parent);
    ctx->child = L; // Remains rooted on the parent's stack until task.defer owns it.
    if (!L)
    {
        report("lua_newthread failed");
        command_status = 1;
        return;
    }
    if (!vm_set_thread_identity(L, 8))
    { report("cannot initialize executor thread context"); command_status = 1; return; }
    uint64_t effective = g_engine.effective_capabilities(L);
    pthread_mutex_lock(&queue_mutex);
    ctx->capabilities = capability_value(effective);
    pthread_mutex_unlock(&queue_mutex);
    if (!ctx->capabilities)
    { report("capability metadata capacity exceeded"); command_status = 1; return; }
    void* shared = make_environment(L, parent);
    register_workspace(L, *(void**)((uint8_t*)L + VM_L_ENV));
    register_http_bridge(L, *(void**)((uint8_t*)L + VM_L_ENV));
    register_vm_api(L, *(void**)((uint8_t*)L + VM_L_ENV));
#if BASALT_ARM_744 && BASALT_DEV_BRIDGE && BASALT_ARM_744_FULL
    register_editor_ui_api(L, *(void**)((uint8_t*)L + VM_L_ENV));
#endif
    const uint64_t* trusted = trusted_capability_value(effective);
    if (!trusted) { report("trusted capability storage exhausted"); command_status = 1; return; }
    int status = load_embedded_bytecode(L, "=basalt.scheduler", EMBEDDED_SCHEDULER, trusted);
    int proto_ready = !status && prepare_loaded_closure(L, trusted);
    if (status != 0 || !proto_ready)
    {
        if (status) report_vm_error(L, "scheduler load");
        command_status = 1;
        return;
    }
    status = protected_call(L, 0, 1); // Produces runner(f, done).
    if (status)
    {
        report_vm_error(L, "scheduler init");
        command_status = status;
        return;
    }
    char chunkname[100];
    snprintf(chunkname, sizeof(chunkname), "@%s", current_request);
    status = bytecode_load(L, chunkname, ctx->bytes, ctx->length, ctx->capabilities);
    if (status != 0 || !prepare_loaded_closure(L, ctx->capabilities))
    {
        if (status) report_vm_error(L, "load");
        command_status = 1;
        return;
    }
    g_engine.lua_pushstring(L, current_request);
    g_engine.lua_pushcclosurek(L, scheduled_done, "basalt_done", 1, NULL);
    g_engine.lua_pushstring(L, current_request);
    g_engine.lua_pushcclosurek(L, script_emit, "basalt_output", 1, NULL);
    push_value(L, (RuntimeValue){.value=(uintptr_t)*(void**)((uint8_t*)L + VM_L_ENV), .tag=7});
    g_engine.lua_pushcclosurek(L, script_loadstring, "loadstring", 1, NULL);
    push_value(L, (RuntimeValue){.value=(uintptr_t)shared, .tag=7});
    status = protected_call(L, 5, 0);
    if (status)
    {
        report_vm_error(L, "scheduler dispatch");
        command_status = status;
        return;
    }
    command_status = 0;
    report("script scheduled");
#endif
}
#endif

static void schedule_editor(void* L, void* data)
{
    boot_trace("schedule_editor enter");
    void* thread = new_executor_thread(L);
    if (!thread) { boot_trace("no thread"); return; }
#if !BASALT_ARM_744
    *(void**)((uint8_t*)thread + 8) = *(void**)((uint8_t*)L + 8);  /* 743: inherit parent ctx wrapper */
#endif
    boot_trace("thread created");
    if (!vm_set_thread_identity(thread, 8))
    { report("editor bootstrap: cannot initialize thread context"); return; }
    report("TRACE: identity set");
    uint64_t capabilities_value = g_engine.effective_capabilities(thread);
    pthread_mutex_lock(&queue_mutex);
    const uint64_t* capabilities = capability_value(capabilities_value);
    pthread_mutex_unlock(&queue_mutex);
    if (!capabilities) { report("editor bootstrap: capability storage exhausted"); return; }

    void* shared = make_environment(thread, L);
    boot_trace("environment made");
    void* env = *(void**)((uint8_t*)thread + VM_L_ENV);
    register_workspace(thread, env);
    report("TRACE: workspace registered");
    register_http_bridge(thread, env);
    report("TRACE: http registered");
    register_vm_api(thread, env);
    report("TRACE: vm api registered");
    register_editor_ui_api(thread, env);
    report("TRACE: ui api registered");
#if BASALT_DEV_BRIDGE
    table_write(thread, env, "_basaltEditorDiagnostics", (RuntimeValue){.value=1,.tag=1});
#endif
    const uint64_t* trusted = trusted_capability_value(capabilities_value);
    if (!trusted) { report("editor bootstrap: trusted capability storage exhausted"); return; }
    boot_trace("loading scheduler");
    int scheduler_load = load_embedded_bytecode(thread, "=basalt.scheduler", EMBEDDED_SCHEDULER, trusted);
    if (scheduler_load) boot_trace("scheduler bytecode load failed");
    int scheduler_prepare = !scheduler_load && !prepare_loaded_closure(thread, trusted);
    if (scheduler_prepare) boot_trace("scheduler closure prepare failed");
    int scheduler_call = !scheduler_load && !scheduler_prepare && protected_call(thread, 0, 1);
    if (scheduler_call) boot_trace("scheduler closure call failed");
    int editor_load = !scheduler_load && !scheduler_prepare && !scheduler_call &&
        load_embedded_bytecode(thread, "@basalt/editor", EMBEDDED_EDITOR, capabilities);
    if (editor_load) boot_trace("editor bytecode load failed");
    int editor_prepare = !scheduler_load && !scheduler_prepare && !scheduler_call && !editor_load &&
        !prepare_loaded_closure(thread, capabilities);
    if (editor_prepare) boot_trace("editor closure prepare failed");
    if (scheduler_load || scheduler_prepare || scheduler_call || editor_load || editor_prepare)
    {
        report_vm_error(thread, "editor bootstrap");
        return;
    }
#if BASALT_DEV_BRIDGE
    g_engine.lua_pushstring(thread, "auto-editor");
    g_engine.lua_pushcclosurek(thread, scheduled_done, "basalt_done", 1, NULL);
    g_engine.lua_pushstring(thread, "auto-editor");
    g_engine.lua_pushcclosurek(thread, script_emit, "basalt_output", 1, NULL);
#else
    push_value(thread, (RuntimeValue){0});
    push_value(thread, (RuntimeValue){0});
#endif
    g_engine.lua_pushcclosurek(thread, script_loadstring, "loadstring", 1, NULL);
    push_value(thread, (RuntimeValue){.value=(uintptr_t)shared, .tag=7});
    boot_trace("calling scheduler with editor");
    int editor_schedule = protected_call(thread, 5, 0);
    if (editor_schedule)
    { boot_trace("editor scheduler call failed"); report_vm_error(thread, "editor scheduling"); return; }
    mark_editor_vm(L);
    *(int*)data = 1;
    boot_trace("BOOTSTRAP COMPLETE");
}

struct actor_start { struct actor_job* job; uint64_t capabilities; int status; char error[160]; };
static void actor_start_error(void* L, struct actor_start* start, const char* phase)
{
    snprintf(start->error,sizeof(start->error),"Actor %s failed",phase);
    if ((uintptr_t)value_top(L)<=(uintptr_t)*(void**)((uint8_t*)L+0x60)) return;
    RuntimeValue value=value_top(L)[-1];
    if (value.tag==6 && value.value) {
        size_t length=*(uint32_t*)(value.value+0x14);
        if (length>sizeof(start->error)-1) length=sizeof(start->error)-1;
        memcpy(start->error,(void*)(value.value+0x18),length);
        start->error[length]=0;
    }
}
static void actor_start_on_child(void* parent, void* data)
{
    struct actor_start* start=data;
    void* thread=new_executor_thread(parent);
    if (thread && start->capabilities) {
        uintptr_t extra=*(uintptr_t*)((uint8_t*)thread+0x80);
        if (extra) *(uint64_t*)(extra+0x40)=start->capabilities;
    }
    if (!thread || !vm_set_thread_identity(thread,8))
    { actor_start_error(parent,start,"thread creation");return; }
    uint64_t effective=g_engine.effective_capabilities(thread);
    pthread_mutex_lock(&queue_mutex);
    const uint64_t* capabilities=capability_value(effective);
    const uint64_t* trusted=trusted_capability_value(effective);
    pthread_mutex_unlock(&queue_mutex);
    if (!capabilities || !trusted) { actor_start_error(thread,start,"capabilities");return; }
    void* shared=make_environment(thread,parent);
    void* env=*(void**)((uint8_t*)thread+0x80);
    table_write(thread,shared,"__basalt_actor_runtime",(RuntimeValue){.value=1,.tag=1});
    char actor_key[32];
    snprintf(actor_key,sizeof(actor_key),"%016llx",(unsigned long long)start->job->actor);
    table_write(thread,shared,"__basalt_actor_address",(RuntimeValue){
        .value=(uintptr_t)g_engine.new_string(thread,actor_key,strlen(actor_key)),.tag=6});
    register_workspace(thread,env);
    register_http_bridge(thread,env);
    register_vm_api(thread,env);
    if (load_embedded_bytecode(thread,"=basalt.scheduler",EMBEDDED_SCHEDULER,trusted) ||
        !prepare_loaded_closure(thread,trusted) || protected_call(thread,0,1))
    { actor_start_error(thread,start,"runtime initialization");return; }
    if (bytecode_load(thread,"=run_on_actor",(uint8_t*)start->job->source,start->job->length,capabilities) ||
        !prepare_loaded_closure(thread,capabilities))
    { actor_start_error(thread,start,"bytecode loading");return; }
    push_value(thread,(RuntimeValue){0}); // completion callback
    push_value(thread,(RuntimeValue){0}); // diagnostics stay inside the Actor VM
    push_value(thread,(RuntimeValue){.value=(uintptr_t)env,.tag=7});
    g_engine.lua_pushcclosurek(thread,script_loadstring,"loadstring",1,NULL);
    push_value(thread,(RuntimeValue){.value=(uintptr_t)shared,.tag=7});
    if (protected_call(thread,5,0)) { actor_start_error(thread,start,"dispatch");return; }
    start->status=1;
}

static void finish_actor_job(void* parent, struct actor_job* job, uint64_t capabilities)
{
    struct actor_start start={.job=job,.capabilities=capabilities};
    uintptr_t global=*(uintptr_t*)((uint8_t*)parent+0x50);
    size_t* threshold=(size_t*)(global+0x8);
    size_t old_limit=*threshold;
    size_t offset=*(uintptr_t*)((uint8_t*)parent+0x58)-*(uintptr_t*)((uint8_t*)parent+0x60);
    *threshold=SIZE_MAX;
    int status=g_engine.luaD_pcall(parent,actor_start_on_child,&start,offset,0);
    *threshold=old_limit;
    vm_sync_thread_context(parent);
    if (status && !start.error[0]) actor_start_error(parent,&start,"protected scheduling");
    restore_stack_top((uint8_t*)parent,offset);
    pthread_mutex_lock(&actor_mutex);
    job->state=!status && start.status ? 3 : 4;
    if (job->state==4) snprintf(job->error,sizeof(job->error),"%s",start.error[0] ? start.error : "Actor did not accept source");
    free(job->source);job->source=NULL;
    g_engine.release_shared(job->shared);job->shared[0]=job->shared[1]=0;
    pthread_mutex_unlock(&actor_mutex);
}
static void process_actor_job(void* parent, uintptr_t actor)
{
    struct actor_job* job=NULL;
    pthread_mutex_lock(&actor_mutex);
    for (unsigned i=0;i<32;++i) if (actor_jobs[i].state==1 && actor_jobs[i].actor==actor) {
        job=&actor_jobs[i];job->state=2;
        __atomic_fetch_sub(&actor_pending,1,__ATOMIC_RELEASE);
        break;
    }
    pthread_mutex_unlock(&actor_mutex);
    if (job) finish_actor_job(parent,job,g_engine.effective_capabilities(parent));
}

static void process_cached_actor_job(void* current)
{
    if (!__atomic_load_n(&actor_pending,__ATOMIC_ACQUIRE)) return;
    uintptr_t model=game_identity(current), main=0;
    struct actor_job* job=NULL;
    uint64_t capabilities=0;
    pthread_mutex_lock(&actor_mutex);
    for (unsigned i=0;i<32 && !job;++i) {
        struct actor_job* candidate=&actor_jobs[i];
        if (candidate->state!=1 || actor_clock()-candidate->created<300) continue;
        for (unsigned n=0;n<observed_actor_count;++n) {
            struct observed_actor_vm* vm=&observed_actor_vms[n];
            uintptr_t owner=0;
            if (candidate->actor!=vm->actor || vm->model!=model ||
                actor_clock()-vm->last_resume<300 || !vm->main ||
                !read_vm(vm->main+0x18,&owner,8) || owner!=vm->global) continue;
            job=candidate; main=vm->main; capabilities=vm->capabilities; job->state=2;
            __atomic_fetch_sub(&actor_pending,1,__ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&actor_mutex);
    if (job) {
        report("scheduling on idle Actor VM");
        finish_actor_job((void*)main,job,capabilities);
    }
}

#if BASALT_DEV_BRIDGE
#if BASALT_SCHEDULER_ENABLED
static void schedule_bytecode(void* L, const uint8_t* bytes, size_t length)
{
    command_status = 1;
    if (!bytes || !length || length > COMMAND_MAX_BYTES - 9)
    {
        report("invalid scheduled bytecode size");
        return;
    }
    uint8_t* state = L;
    size_t offset = *(uintptr_t*)(state + VM_L_TOP) - *(uintptr_t*)(state + VM_L_STACK);
    struct schedule_context ctx = {bytes, length, NULL, NULL};
#if BASALT_ARM_744
    int status = g_engine.luaD_pcall(L, schedule_on_child, &ctx, offset, 0);
#else
    uintptr_t global = *(uintptr_t*)(state + VM_L_GLOBAL);
    size_t* threshold = (size_t*)(global + 0x8);
    size_t saved_threshold = *threshold;
    *threshold = SIZE_MAX;
    int status = g_engine.luaD_pcall(L, schedule_on_child, &ctx, offset, 0);
    *threshold = saved_threshold;
#endif
    vm_sync_thread_context(L);
    if (status)
    {
        report_vm_error(L, "scheduler");
        command_status = status;
    }
    restore_stack_top(state, offset);
}
#endif

static void execute_bytecode(void* L, const uint8_t* bytes, size_t length, int mode)
{
    command_status = 1;
    if (length > 16 * 1024 * 1024 || length == 0)
    {
        report("rejected oversized or empty bytecode");
        return;
    }

    if (!probe_protected_call(L))
    {
        report("protected-call probe failed; bytecode execution skipped");
        return;
    }
#if BASALT_ARM_744 && BASALT_DEV_BRIDGE
    {
        if (mode == 0) {
            void* probe = g_engine.new_string(L, "__basalt_shared_v3", 18);
            char line[96]; snprintf(line, sizeof(line), "744 parent intern=%p", probe);
            report(line);
        }
        uintptr_t g = 0;
        read_vm((uintptr_t)L + VM_L_GLOBAL, &g, 8);
        for (size_t off = 0; g && off < 0xb0; off += 8) {
            uintptr_t value = 0;
            if (read_vm(g + off, &value, 8)) {
                char line[96];
                snprintf(line, sizeof(line), "744 global+%02zx=%p", off, (void*)value);
                report(line);
            }
        }
    }
#endif

    uint8_t* state = L;
    uint8_t* original_top = *(uint8_t**)(state + VM_L_TOP);
    size_t original_stack_offset = original_top - *(uint8_t**)(state + VM_L_STACK);
    int before = g_engine.lua_gettop(L);
    int status;
    if (mode == 0)
    {
#if BASALT_ARM_744
        /* 2.744's engine loader is available. The former hand-built proto
           layout is incompatible with this engine and must not be invoked. */
        status = engine_load_submitted(L, "=basalt", (const char*)bytes, length, 0);
#else
        uint64_t effective = g_engine.effective_capabilities(L);
        pthread_mutex_lock(&queue_mutex);
        const uint64_t* capabilities = capability_value(effective);
        pthread_mutex_unlock(&queue_mutex);
        status = capabilities ? bytecode_load(L, "=basalt", bytes, length, capabilities) : 1;
#endif
    }
    else
        status = engine_load_submitted(L, "=basalt", (const char*)bytes, length, 0);
    int after = g_engine.lua_gettop(L);

    char message[160];
    snprintf(message, sizeof(message), "load status=%d top=%d->%d version=%u",
             status, before, after, bytes[0]);
    report(message);

    if (status != 0 || after != before + 1)
    {
        if (after == before + 1)
            report_vm_error(L, "load");
        restore_stack_top(state, original_stack_offset);
        return;
    }

    // Only run a genuine Luau closure; an error message is not executable.
    uint8_t* top = *(uint8_t**)((uint8_t*)L + VM_L_TOP);
    uint32_t tag;
    memcpy(&tag, top - 4, sizeof(tag));
    if (tag != 8) // LUA_TFUNCTION for this Roblox build
    {
        snprintf(message, sizeof(message), "loaded value has unexpected tag=%u", tag);
        report(message);
        restore_stack_top(state, original_stack_offset);
        return;
    }

    if (mode != 0)
    {
        inspect_loaded_function(top);
        if (mode == 2)
            compare_loaded_function(L, top);
        restore_stack_top(state, original_stack_offset);
        command_status = 0;
        return;
    }

    report("calling loaded closure via luaD_pcall");
    int result = protected_call(L, 0, 0);
    command_status = result;
    snprintf(message, sizeof(message), "protected execution status=%d", result);
    report(message);
    if (result != 0)
    {
        report_vm_error(L, "execution");
        restore_stack_top(state, original_stack_offset);
    }
}

static void process_command(void* L, struct pending_command item)
{
    snprintf(current_request, sizeof(current_request), "%s", item.id);
    command_status = 0;
    report("request started");
    struct command cmd = parse_command(item.bytes, item.length);
    switch (cmd.type)
    {
    case COMMAND_PING:
        report("PING received on a VM thread");
        break;
    case COMMAND_PROBE:
        probe_protected_call(L);
        break;
    case COMMAND_CALL_PROBE:
        probe_native_call(L);
        break;
    case COMMAND_CONTEXT:
        inspect_context(L);
        break;
    case COMMAND_BYTECODE:
        execute_bytecode(L, cmd.data, cmd.size, 0);
        break;
    case COMMAND_SCHEDULE:
#if BASALT_SCHEDULER_ENABLED
        schedule_bytecode(L, cmd.data, cmd.size);
#else
        command_status = 1;
        report("scheduler path is disabled for this Roblox 2.742 port; submit with direct execution");
#endif
        break;
    case COMMAND_INSPECT:
        execute_bytecode(L, cmd.data, cmd.size, 1);
        break;
    case COMMAND_COMPARE:
        execute_bytecode(L, cmd.data, cmd.size, 2);
        break;
    default:
        command_status = 1;
        report("unknown command; expected PING or EXEC_BIN + binary bytecode");
        break;
    }
    char message[96];
    snprintf(message, sizeof(message), "request completed status=%d", command_status);
    report(message);
    current_request[0] = '\0';
    free(item.bytes);
}
#endif


static RuntimeValue environment_lookup(void* L, void* environment, const char* name)
{
    RuntimeValue key = {.value=(uintptr_t)g_engine.new_string(L, name, strlen(name)), .tag=6};
    RuntimeValue index = {.value=(uintptr_t)g_engine.new_string(L, "__index", 7), .tag=6};
#if BASALT_DEV_BRIDGE
    static unsigned game_lookups;
    unsigned trace = !strcmp(name, "game") ? __atomic_fetch_add(&game_lookups, 1, __ATOMIC_RELAXED) : UINT_MAX;
#endif
    for (unsigned depth = 0; environment && depth < 8; ++depth)
    {
        RuntimeValue value = *(const RuntimeValue*)g_engine.table_get(environment, &key);
#if BASALT_DEV_BRIDGE
        if (trace < 8) LOGI("742 lookup game: depth=%u table=%p value_tag=%u value=%p",
                            depth, environment, value.tag, (void*)value.value);
#endif
        if (value.tag) return value;
        void* metatable = *(void**)((uint8_t*)environment + VM_TABLE_METATABLE);
#if BASALT_DEV_BRIDGE
        if (trace < 8) LOGI("742 lookup game: mt=%p", metatable);
#endif
        if (!metatable) break;
        RuntimeValue inherited = *(const RuntimeValue*)g_engine.table_get(metatable, &index);
#if BASALT_DEV_BRIDGE
        if (trace < 8) LOGI("742 lookup game: inherited_tag=%u inherited=%p",
                            inherited.tag, (void*)inherited.value);
#endif
        if (inherited.tag != 7) break;
        environment = (void*)inherited.value;
    }
    return (RuntimeValue){0};
}

static RuntimeValue active_script_lookup(void* L, const char* name)
{
    void* thread_environment = *(void**)((uint8_t*)L + VM_L_ENV);
    RuntimeValue value = environment_lookup(L, thread_environment, name);
    if (value.tag) return value;

    uintptr_t ci = *(uintptr_t*)((uint8_t*)L + VM_L_CI);
    uintptr_t first = *(uintptr_t*)((uint8_t*)L + VM_L_BASE_CI);
    for (unsigned depth = 0; depth < 64 && ci >= first && ci - first < 0x1000;
         ++depth, ci -= 0x30)
    {
        uintptr_t slot = 0, environment = 0;
        RuntimeValue function = {0};
        if (!read_vm(ci + 0x18, &slot, 8) || !slot ||
            !read_vm(slot, &function, sizeof(function)) || function.tag != 8 ||
            !function.value || !read_vm(function.value + 0x10, &environment, 8) ||
            !environment || (void*)environment == thread_environment) continue;
        value = environment_lookup(L, (void*)environment, name);
        if (value.tag) return value;
    }
    return (RuntimeValue){0};
}

static int game_context(void* L)
{
    RuntimeValue game = raw_active_lookup(L, "game");
#if BASALT_DEV_BRIDGE
    void* environment = *(void**)((uint8_t*)L + VM_L_ENV);
    static unsigned game_probes;
    unsigned trace = __atomic_fetch_add(&game_probes, 1, __ATOMIC_RELAXED);
    if (trace < 8) LOGI("742 game context: env=%p tag=%u value=%p",
                        environment, game.tag, (void*)game.value);
#if BASALT_ARM_744
    if (trace == 0 && game.tag == 9 && game.value) {
        uintptr_t candidate = 0;
        if (read_vm(game.value + 0x10, &candidate, 8) && candidate) {
            for (unsigned off = 0; off <= 0xe0; off += 8) {
                uintptr_t pointer = 0;
                uint8_t data[24] = {0};
                if (!read_vm(candidate + off, &pointer, 8) || !pointer ||
                    !read_vm(pointer, data, sizeof(data))) continue;
                LOGI("744 model field=%x pointer=%p data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    off,(void*)pointer,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7],
                    data[8],data[9],data[10],data[11],data[12],data[13],data[14],data[15]);
            }
        }
    }
#endif
#endif
    if (game.tag != 9) return 0;
    uintptr_t instance = 0, name = 0;
    uint8_t storage[24] = {0};
    if (!read_vm(game.value + 0x10, &instance, 8) ||
        !read_vm(instance + VM_INSTANCE_NAME_SLOT, &name, 8) || !name ||
        !read_vm(name + VM_INSTANCE_NAME_DATA, storage, sizeof(storage))) {
#if BASALT_DEV_BRIDGE
        if (trace < 8) LOGI("742 game context: instance=%p name=%p unreadable",
                            (void*)instance, (void*)name);
#endif
        return 0;
    }
#if BASALT_DEV_BRIDGE && BASALT_ARM_744
    if (trace < 8) {
        uint8_t name_header[48] = {0};
        read_vm(name, name_header, sizeof(name_header));
        LOGI("744 game name probe instance=%p object=%p bytes=%02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x",
            (void*)instance, (void*)name,
            name_header[0],name_header[1],name_header[2],name_header[3],
            name_header[4],name_header[5],name_header[6],name_header[7],
            name_header[8],name_header[9],name_header[10],name_header[11],
            name_header[12],name_header[13],name_header[14],name_header[15],
            name_header[16],name_header[17],name_header[18],name_header[19],
            name_header[20],name_header[21],name_header[22],name_header[23]);
    }
#endif
    char text[32] = {0};
    size_t length;
    if (storage[0] & 1)
    {
        uintptr_t content;
        memcpy(&length, storage + 8, 8);
        memcpy(&content, storage + 16, 8);
        if (length >= sizeof(text) || !read_vm(content, text, length)) return 0;
    }
    else
    {
        length = storage[0] >> 1;
        if (length > 22) return 0;
        memcpy(text, storage + 1, length);
    }
    // The running experience and the app shell are separate DataModels in this
    // Android build. Never dispatch experience commands into LuaApp.
#if BASALT_DEV_BRIDGE
    if (trace < 8) LOGI("742 game context: instance=%p name=%p length=%zu text=%s",
                        (void*)instance, (void*)name, length, text);
#endif
    return length == 3 && !memcmp(text, "Ugc", 3);
}

static uintptr_t game_identity(void* L)
{
    RuntimeValue game = raw_active_lookup(L, "game");
    if (game.tag != 9) return 0;
    uintptr_t identity = 0;
    if (!read_vm(game.value + 0x10, &identity, 8)) return 0;
    return identity;
}

static uintptr_t script_actor_context(void* L)
{
    RuntimeValue script = raw_active_lookup(L, "script");
    uintptr_t instance = 0;
    uint32_t size = 0;
    if (script.tag != 9 || !read_vm(script.value + 4, &size, 4) || size != 16 ||
        !read_vm(script.value + 0x10, &instance, 8)) return UINTPTR_MAX;
    return instance_actor_owner(instance, read_vm);
}

void hook_capture_pre(void* L, void* second)
{
    (void)second;
    if (!L)
        return;

#if BASALT_DEV_BRIDGE
#if BASALT_ARM_744
    static unsigned trace_744_count;
    if (__atomic_fetch_add(&trace_744_count, 1, __ATOMIC_RELAXED) < 12) {
        uintptr_t env = 0, global = 0, base = 0, top = 0, stack = 0;
        uintptr_t ci = 0, first = 0, extra = 0, main = 0;
        uint8_t env_header[8] = {0};
        read_vm((uintptr_t)L + VM_L_ENV, &env, 8);
        read_vm((uintptr_t)L + VM_L_GLOBAL, &global, 8);
        read_vm((uintptr_t)L + VM_L_BASE, &base, 8);
        read_vm((uintptr_t)L + VM_L_TOP, &top, 8);
        read_vm((uintptr_t)L + VM_L_STACK, &stack, 8);
        read_vm((uintptr_t)L + VM_L_CI, &ci, 8);
        read_vm((uintptr_t)L + VM_L_BASE_CI, &first, 8);
        read_vm((uintptr_t)L + VM_L_EXTRA, &extra, 8);
        if (env) read_vm(env, env_header, sizeof(env_header));
        if (global) read_vm(global + VM_GLOBAL_MAINTHREAD, &main, 8);
        LOGI("744 VM L=%p env=%p envheader=%02x,%02x,%02x,%02x global=%p main=%p ci=%p first=%p base=%p stack=%p top=%p extra=%p",
             L, (void*)env, env_header[0],env_header[1],env_header[2],env_header[3],
             (void*)global, (void*)main, (void*)ci, (void*)first,
             (void*)base, (void*)stack, (void*)top, (void*)extra);
    }
#else
    static uintptr_t traced_globals[32];
    uintptr_t words[18] = {0};
    if (read_vm((uintptr_t)L, words, sizeof(words)) && words[3]) {
        for (unsigned i = 0; i < 32; ++i) {
            if (__atomic_load_n(&traced_globals[i], __ATOMIC_ACQUIRE) == words[3]) break;
            uintptr_t empty = 0;
            if (__atomic_compare_exchange_n(&traced_globals[i], &empty, words[3], 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                uint64_t extra[12] = {0};
                if (words[16]) read_vm(words[16], extra, sizeof(extra));
                LOGI("742 VM global=%p L=%p top=%p base=%p stack=%p env=%p extra=%p",
                     (void*)words[10], L, (void*)words[11], (void*)words[9],
                     (void*)words[12], (void*)words[16], (void*)words[15]);
                LOGI("742 extra: 20=%llx 28=%llx 30=%llx 38=%llx 40=%llx 48=%llx 50=%llx 58=%llx",
                     (unsigned long long)extra[4], (unsigned long long)extra[5],
                     (unsigned long long)extra[6], (unsigned long long)extra[7],
                     (unsigned long long)extra[8], (unsigned long long)extra[9],
                     (unsigned long long)extra[10], (unsigned long long)extra[11]);
                break;
            }
        }
    }
#endif
    int pending = __atomic_load_n(&queue_length, __ATOMIC_ACQUIRE) != 0;
#else
    const int pending = 0;
#endif
    ++editor_poll;
    unsigned pending_actors=__atomic_load_n(&actor_pending,__ATOMIC_ACQUIRE);
    int maintenance = editor_poll < 16 || (editor_poll & 31u) == 0;
    /*
     * This hook runs for ordinary Luau calls. Actor discovery and DataModel
     * probing are maintenance work, not per-call work; sampling them keeps the
     * hot path to a few atomic loads when there is nothing to execute.
     */
    if (maintenance && __atomic_load_n(&observed_actor_count,__ATOMIC_ACQUIRE) < 32) {
        uintptr_t global=*(uintptr_t*)((uint8_t*)L+VM_L_GLOBAL);
        if (__atomic_load_n(&active_game_global,__ATOMIC_ACQUIRE) &&
            global!=__atomic_load_n(&active_game_global,__ATOMIC_ACQUIRE) && game_context(L)) {
            uintptr_t actor=script_actor_context(L);
            if (actor && actor!=UINTPTR_MAX) {
                pthread_mutex_lock(&actor_mutex);
                unsigned i=0;
                for (;i<observed_actor_count;++i) if (observed_actor_vms[i].global==global) break;
                if (i==observed_actor_count && observed_actor_count<32) {
                    observed_actor_vms[observed_actor_count]=(struct observed_actor_vm){actor,global,
                        *(uintptr_t*)(global+0x2e8),game_identity(L),actor_clock(),g_engine.effective_capabilities(L)};
                    __atomic_fetch_add(&observed_actor_count,1,__ATOMIC_RELEASE);
#if BASALT_DEV_BRIDGE
                    char detail[160];
                    snprintf(detail,sizeof(detail),"actor VM seen: actor=%p global=%p main=%p thread=%p",
                        (void*)actor,(void*)global,*(void**)(global+0x2e8),L);
                    report(detail);
#endif
                }
                else if (i<observed_actor_count) observed_actor_vms[i].last_resume=actor_clock();
                pthread_mutex_unlock(&actor_mutex);
            }
        }
    }
    if (!pending && !pending_actors && !maintenance) return;

    // LocalScripts own the game's VM; identity-3 CoreScripts have a different
    // registry and Instance metatable. Create our elevated child in the game
    // VM so hooks and enumeration can observe the game's own scripts.
    {
        uintptr_t envv = *(uintptr_t*)((uint8_t*)L + VM_L_ENV);
        uint8_t envtt = 0;
        if (!envv || !read_vm(envv + VM_GCO_TYPE, &envtt, 1) || envtt != 7)
            return;
    }
    if (!game_context(L))
        return;
#if BASALT_ARM_744
    // Both CoreScripts and LocalScripts resume in the experience DataModel,
    // but their VM registries and module caches are separate. Dispatch from
    // an ordinary LocalScript so API enumeration sees the game script state.
    uintptr_t dispatch_extra = 0;
    uint32_t dispatch_identity = 0;
    if (!read_vm((uintptr_t)L + VM_L_EXTRA, &dispatch_extra, sizeof(dispatch_extra)) ||
        !dispatch_extra ||
        !read_vm(dispatch_extra + VM_EXTRA_IDENTITY, &dispatch_identity, sizeof(dispatch_identity)) ||
        dispatch_identity != 2)
        return;
#endif
    if ((editor_poll & 4095u) == 0) boot_trace("gate passed");

#if BASALT_DEV_BRIDGE
    static unsigned frame_reports;
    if (__atomic_fetch_add(&frame_reports, 1, __ATOMIC_RELAXED) < 6) {
        uintptr_t ci = *(uintptr_t*)((uint8_t*)L + VM_L_CI);
        uintptr_t first = *(uintptr_t*)((uint8_t*)L + VM_L_BASE_CI);
        LOGI("742 frames: L=%p ci=%p first=%p env=%p", L, (void*)ci,
              (void*)first, *(void**)((uint8_t*)L + VM_L_ENV));
        for (unsigned depth = 0; depth < 5 && ci >= first && ci - first < 0x1000; ++depth, ci -= 0x30) {
            for (unsigned field = 0; field <= 0x18; field += 8) {
                uintptr_t slot = 0;
                RuntimeValue function = {0};
                uintptr_t closure_env = 0;
                if (!read_vm(ci + field, &slot, 8) || !slot ||
                    !read_vm(slot, &function, sizeof(function))) continue;
                if (function.tag == 8 && function.value)
                    read_vm(function.value + 0x10, &closure_env, 8);
                if (function.tag == 8 || field == 0x18)
                    LOGI("742 frame: depth=%u field=%x slot=%p tag=%u closure=%p env=%p",
                         depth, field, (void*)slot, function.tag,
                         (void*)function.value, (void*)closure_env);
            }
        }
    }
#endif

    int expected = 0;
    if (!__atomic_compare_exchange_n(&dispatching, &expected, 1, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;

    if (!pending && !pending_actors && editor_vm_attached(L))
    {
        __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
        return;
    }
    int in_game = game_context(L);
#if BASALT_DEV_BRIDGE
    static unsigned game_context_reports;
    if (__atomic_fetch_add(&game_context_reports, 1, __ATOMIC_RELAXED) < 8)
        LOGI("742 dispatch: L=%p global=%p game_context=%d pending=%d",
             L, *(void**)((uint8_t*)L+VM_L_GLOBAL), in_game, pending);
#endif
    if ((editor_poll & 4095u) == 0) boot_trace(in_game ? "in_game=1" : "in_game=0");
    if (!in_game)
    {
        __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
        return;
    }
    uintptr_t actor=script_actor_context(L);
#if BASALT_DEV_BRIDGE
    static unsigned actor_context_reports;
    if (__atomic_fetch_add(&actor_context_reports, 1, __ATOMIC_RELAXED) < 8)
        LOGI("742 dispatch: actor=%p model=%p", (void*)actor, (void*)game_identity(L));
#endif
    if (actor && actor!=UINTPTR_MAX)
    {
        if (pending_actors) process_actor_job(L,actor);
        __atomic_store_n(&dispatching,0,__ATOMIC_RELEASE);
        return;
    }
    if (actor!=0 && actor!=UINTPTR_MAX)
    {
        __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
        return;
    }
    if (actor==UINTPTR_MAX && (editor_poll & 4095u) == 0) boot_trace("actor unknown (layout) - continuing");

    uintptr_t model = game_identity(L);
    if (pending_actors) process_cached_actor_job(L);
    // Wait for the same experience DataModel across several dispatches before
    // creating the editor thread in its VM.
    static uintptr_t settle_model;
    static unsigned settle_poll;
    static unsigned settle_hits;
    if (!settle_poll) settle_poll = editor_poll;
    if (model && model == settle_model) { if (settle_hits < 8) ++settle_hits; }
    else { settle_model = model; settle_hits = 0; settle_poll = editor_poll; }
    if (model && settle_hits < 8) { __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE); return; }
    if (model && editor_poll - settle_poll < 128) { __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE); return; }
    // A new game can reuse both the old global and DataModel addresses. Its VM
    // registry is fresh, so it still needs its own editor and queued scripts.
    if (model && (!BASALT_DEV_BRIDGE || (BASALT_ARM_744 && BASALT_ARM_744_FULL)) &&
        BASALT_SCHEDULER_ENABLED && !editor_vm_attached(L) &&
        !editor_bootstrapping && editor_poll >= editor_retry_at)
    {
        boot_trace("bootstrap enter");
        editor_bootstrapping = 1;
        uintptr_t global = *(uintptr_t*)((uint8_t*)L + VM_L_GLOBAL);
        size_t stack_offset = *(uintptr_t*)((uint8_t*)L + VM_L_TOP) - *(uintptr_t*)((uint8_t*)L + VM_L_STACK);
#if !BASALT_ARM_744
        size_t* threshold = (size_t*)(global + 0x8);
        size_t saved_threshold = *threshold;
        *threshold = SIZE_MAX;
#endif
        int scheduled = 0;
        boot_trace("pcalling schedule_editor");
        int status = g_engine.luaD_pcall(L, schedule_editor, &scheduled, stack_offset, 0);
        boot_trace(status ? "pcall status!=0" : "pcall done");
#if !BASALT_ARM_744
        *threshold = saved_threshold;
#endif
        vm_sync_thread_context(L);
        if (status) report_vm_error(L, "editor launch");
        else if (scheduled) report("editor VM attached");
        restore_stack_top(L, stack_offset);
        editor_retry_at = editor_poll + 4096;
        editor_bootstrapping = 0;
    }

#if BASALT_DEV_BRIDGE
    if (!__atomic_load_n(&queue_length, __ATOMIC_ACQUIRE))
    { __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE); return; }

    struct pending_command batch[16];
    pthread_mutex_lock(&queue_mutex);
    size_t count = queue_length;
    if (count)
    {
        memcpy(batch, queue, count * sizeof(*batch));
        __atomic_store_n(&queue_length, 0, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&queue_mutex);

    for (size_t i = 0; i < count; ++i)
        process_command(L, batch[i]);
#endif

    __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
}

static void* hook_capture_resume(void* L, void* second)
{
    hook_capture_pre(L, second);
    return second;
}

static void* hook_capture_callint(void* L, void* function)
{
    uintptr_t previous = *(uintptr_t*)((uint8_t*)L + 0x60);
    uintptr_t address = (uintptr_t)function;
    uintptr_t top = *(uintptr_t*)((uint8_t*)L + VM_L_TOP);
    uintptr_t offset = address >= previous && address <= top ? address - previous : UINTPTR_MAX;
    hook_capture_pre(L, function);
    uintptr_t current = *(uintptr_t*)((uint8_t*)L + 0x60);
    return offset != UINTPTR_MAX && current != previous ? (void*)(current + offset) : function;
}

#if BASALT_DEV_BRIDGE
static void scan_channel(void)
{
    DIR* dir = opendir(IN_DIR);
    if (!dir)
        return;

    struct dirent* entry;
    while ((entry = readdir(dir)))
    {
        if (entry->d_name[0] == '.')
            continue;
        if (strlen(entry->d_name) >= sizeof(queue[0].id) ||
            strspn(entry->d_name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(entry->d_name))
            continue;
        if (__atomic_load_n(&queue_length, __ATOMIC_ACQUIRE) >= 16)
            break; // Leave the file for the next scan rather than dropping it.

        char path[512];
        snprintf(path, sizeof(path), IN_DIR "/%s", entry->d_name);
        FILE* in = fopen(path, "rb");
        if (!in)
            continue;

        int consumed = 0;
        if (fseek(in, 0, SEEK_END) == 0)
        {
            long size = ftell(in);
            if (size > 0 && size <= COMMAND_MAX_BYTES && fseek(in, 0, SEEK_SET) == 0)
            {
                uint8_t* bytes = malloc((size_t)size);
                if (bytes && fread(bytes, 1, (size_t)size, in) == (size_t)size)
                {
                    pthread_mutex_lock(&queue_mutex);
                    size_t next = __atomic_load_n(&queue_length, __ATOMIC_RELAXED);
                    if (next < 16)
                    {
                        queue[next] = (struct pending_command){.bytes=bytes, .length=(size_t)size};
                        snprintf(queue[next].id, sizeof(queue[next].id), "%s", entry->d_name);
                        ++next;
                        __atomic_store_n(&queue_length, next, __ATOMIC_RELEASE);
                        bytes = NULL;
                        consumed = 1;
                    }
                    pthread_mutex_unlock(&queue_mutex);
                }
                free(bytes);
            }
        }
        fclose(in);
        if (consumed)
            unlink(path);
    }
    closedir(dir);
}

static void* channel_thread(void* ignored)
{
    (void)ignored;
    for (;;)
    {
        scan_channel();
        usleep(200000);
    }
    return NULL;
}

static void* start_channel(void* ignored)
{
    (void)ignored;
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        mkdir(CHANNEL_DIR, 0700);
        mkdir(IN_DIR, 0700);
        mkdir(OUT_DIR, 0700);
        DIR* directory = opendir(IN_DIR);
        if (directory)
        {
            closedir(directory);
            break;
        }
        usleep(500000);
    }
    char path[512];
    snprintf(path, sizeof(path), OUT_DIR "/session.txt");
    FILE* info = fopen(path, "w");
    if (info)
    {
        fprintf(info, "pid=%d\nprotocol=3\nloader=bounded-v9\nschedule=1\nvm=game\n", getpid());
        fclose(info);
    }
    return channel_thread(NULL);
}
#endif

__attribute__((constructor(200)))
static void basalt_start(void)
{
    char command[256] = {0};
    FILE* process = fopen("/proc/self/cmdline", "rb");
    if (!process) return;
    size_t length = fread(command, 1, sizeof(command), process);
    fclose(process);
    if (!executor_is_main_process(command, length)) return;
#if defined(__x86_64__)
    // 2.744.1081 x86_64 native emulator builds use the engine-driven direct
    // path; the ARM64 runtime protection pins are not ported yet.
    if (engine_init() != 0)
        return;
    extern int x86_executor_start(void);
    if (x86_executor_start() != 0)
        LOGE("x86 executor start failed");
    return;
#endif
#if !BASALT_DEV_BRIDGE
    cleanup_legacy_diagnostics("/sdcard/Android/data/com.roblox.client/files/basalt");
#endif
    if (engine_init() != 0)
        return;
    if (runtime_protection_init() != 0)
        return;
    if (engine_install_bytecode_gate() != 0) {
        LOGE("bytecode gate validation failed");
        return;
    }

    if (hook_arm64_inline(g_engine.resume_entry, (void*)hook_capture_resume) != HOOK_OK)
    {
        LOGE("could not install dispatch hook");
        return;
    }

    // The coroutine-resume wrapper can be idle for minutes in some experiences.
    // luaD_callint receives the same live lua_State during ordinary Luau calls.
    // The dispatching flag in hook_capture_pre prevents recursive queue draining.
    if (hook_arm64_inline((uint64_t)g_engine.luaD_callint, (void*)hook_capture_callint) != HOOK_OK)
        LOGE("could not install luaD_callint fallback dispatch hook");

#if BASALT_DEV_BRIDGE
    pthread_t thread;
    if (pthread_create(&thread, NULL, start_channel, NULL) == 0)
        pthread_detach(thread);
    report("dispatch hook ready; request-log protocol v3; bounded v9 loader");
#endif
}
