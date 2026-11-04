// Decode x86_64 (2.744.1081): engine-driven load/pcall path.
#include "engine.h"
#include "hook.h"
#include "build_config.h"
#include "compiler_bridge.h"
#include "http_bridge.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__x86_64__)

#define CHANNEL "/sdcard/Android/data/com.roblox.client/files/decode"
#define IN_DIR CHANNEL "/in"
#define OUT_DIR CHANNEL "/out"
#define SESSION OUT_DIR "/session.txt"
#define LOG_PATH OUT_DIR "/log.txt"
#define WORKSPACE "/storage/emulated/0/Decode"
#define AUTOEXEC WORKSPACE "/autoexec"

/* 2.744.1081 x86_64 lua_State offsets measured live. */
#define L_TOP 0x70
#define L_GLOBAL 0x78
#define L_CI 0x50
#define L_BASE_CI 0x18
#define CLOSURE_ENV 0x18

typedef struct { uint64_t value; uint32_t extra, tag; } Value;

/* validator: `return script ~= nil and 1 or 0`; true only when a candidate
   env table is a real script environment (only script envs expose `script`). */
static const uint8_t donor_probe_bytecode[] = {9,3,1,6,115,99,114,105,112,116,0,1,3,0,0,1,2,0,10,163,0,0,0,53,1,0,114,0,0,0,0,198,2,0,0,241,1,3,0,2,0,0,0,111,0,1,0,43,0,1,0,111,0,2,0,130,0,2,0,3,3,1,2,0,0,0,0,0,0,240,63,2,0,0,0,0,0,0,0,0,0,1,0,0,0,0};
static uintptr_t donor_env;
static uintptr_t donor_mask_ptr;
static uintptr_t donor_global;
static _Thread_local const uint8_t* loading_bytecode;
static int (*original_bytecode_gate)(void*, const char*, const char*, size_t, int);

struct command_context {
    const uint8_t* data;
    size_t size;
    const char* token;
    char result[512];
    int load_status;
    int has_result;
    uintptr_t original_top;   // L->top before execution; restored when done
    uintptr_t stack_base;     // L->stack, for the pcall restore offset
    uintptr_t security, extra;
    uint32_t old_identity, old_extra_identity;
    int identity_changed, job;
};

static struct { int active; char token[80]; } jobs[32];
static const char scheduler_source[] =
    "return function(f,done,identity,compile,httpStart,httpPoll,read,write)\n"
    "local env=setmetatable({}, {__index=getfenv(f)})\n"
    "env.getthreadidentity=identity\n"
    "env.identifyexecutor=function() return 'Decode','1.0' end\n"
    "env.getexecutorname=env.identifyexecutor\n"
    "env.identify_executor=env.identifyexecutor\n"
    "env.loadstring=function(s,n) return compile(s,n,env) end\n"
    "env.readfile=function(path) local data,err=read(path); if data==nil then error(err,2) end; return data end\n"
    "env.writefile=function(path,data) local ok,err=write(path,data); if not ok then error(err,2) end end\n"
    "env.httpget=function(url)\n"
    "assert(type(url)=='string','HttpGet expects a URL')\n"
    "local service=game:GetService('HttpService')\n"
    "local id,problem=httpStart(service:JSONEncode({Url=url,Method='GET'}))\n"
    "assert(id,problem)\n"
    "while true do\n"
    "local ready,body,err=httpPoll(id)\n"
    "if ready then\n"
    "assert(body,err)\n"
    "local response=service:JSONDecode(body)\n"
    "assert(response.Success,'HTTP '..tostring(response.StatusCode)..': '..tostring(response.StatusMessage))\n"
    "assert(type(response.Body)=='string','HTTP response is not text')\n"
    "return response.Body\n"
    "end\n"
    "task.wait(0.05)\n"
    "end\n"
    "end\n"
    "setfenv(f,env)\n"
    "task.defer(function() local r=table.pack(xpcall(f,debug.traceback)); "
    "done(table.unpack(r,1,r.n)) end)\n"
    "end";

static int dispatching;
static unsigned resume_count;
static int channel_ready;
static int autoexec_bootstrapped;

static void report(const char* format, ...) {
#if BASALT_DEV_BRIDGE
    va_list args;
    va_start(args, format);
    FILE* log = fopen(LOG_PATH, "a");
    if (log) { vfprintf(log, format, args); fputc('\n', log); fclose(log); }
    va_end(args);
#else
    (void)format;
#endif
}
static void write_result(const char* token, const char* text) {
    char path[256];
    snprintf(path, sizeof(path), OUT_DIR "/%s.cmd.log", token);
    FILE* out = fopen(path, "w");
    if (!out) return;
    fputs(text, out);
    fclose(out);
}

static void describe_value(Value* slot, char* out, size_t capacity) {
    switch (slot->tag) {
    case 0: snprintf(out, capacity, "result nil"); break;
    case 1: snprintf(out, capacity, "result %s", (uint32_t)slot->value ? "true" : "false"); break;
    case 3: {
        double number;
        memcpy(&number, &slot->value, sizeof(number));
        snprintf(out, capacity, "result %.17g", number);
        break;
    }
    case 6: {
        const uint8_t* string = (const uint8_t*)slot->value;
        if (string) {
            uint32_t length;
            memcpy(&length, string + 0x14, sizeof(length));
            if (length > 240) length = 240;
            char text[241];
            memcpy(text, string + 0x18, length);
            text[length] = 0;
            snprintf(out, capacity, "result \"%s\"", text);
        } else snprintf(out, capacity, "result \"\"");
        break;
    }
    default: snprintf(out, capacity, "result tag=%u", slot->tag); break;
    }
}

static int probe_read(uintptr_t address, void* out, size_t size);

static Value* vm_top(void* L) { return *(Value**)((uint8_t*)L + L_TOP); }
static Value* vm_base(void* L) { return *(Value**)((uint8_t*)L + 0x58); }
static void push(void* L, Value v) {
    g_engine.check_stack(L, 1);
    *vm_top(L) = v;
    *(Value**)((uint8_t*)L + L_TOP) += 1;
}
static void push_text(void* L, const char* text) {
    void* s = g_engine.new_string(L, text, strlen(text));
    push(L, (Value){.value=(uintptr_t)s, .tag=6});
}
static uintptr_t identity_context(void) {
    void* (*accessor)(void*) = (void*)(g_engine.base + 0x02DB70B0);
    return (uintptr_t)accessor((void*)(g_engine.base + 0x075C44E8));
}
static void restore_identity(struct command_context* ctx) {
    if (!ctx->identity_changed) return;
    *(uint32_t*)ctx->security = ctx->old_identity;
    *(uint32_t*)(ctx->extra + 0x60) = ctx->old_extra_identity;
    ctx->identity_changed = 0;
}
static void set_identity(void* L, struct command_context* ctx) {
    ctx->security = identity_context();
    ctx->extra = *(uintptr_t*)((uint8_t*)L + 0x28);
    ctx->old_identity = *(uint32_t*)ctx->security;
    ctx->old_extra_identity = *(uint32_t*)(ctx->extra + 0x60);
    ctx->identity_changed = 1;
    *(uint32_t*)ctx->security = 8;
    *(uint32_t*)(ctx->extra + 0x60) = 8;
}
static void prepare_proto(uintptr_t proto, unsigned depth) {
    if (!proto || depth > 128) return;
    *(uintptr_t*)(proto + 0x38) = donor_mask_ptr;
    uint32_t count = *(uint32_t*)(proto + 0xa8);
    uintptr_t* children = *(uintptr_t**)(proto + 0x30);
    if (count > 32768) return;
    for (uint32_t i = 0; i < count; ++i) prepare_proto(children[i], depth + 1);
}
static int load_chunk(void* L, const char* name, const uint8_t* data, size_t size, uintptr_t env) {
    int index = 0;
    if (env) {
        push(L, (Value){.value=env, .tag=7});
        index = (int)(vm_top(L) - vm_base(L));
    }
    const uint8_t* previous = loading_bytecode;
    loading_bytecode = data;
    int status = g_engine.luau_load(L, name, (const char*)data, size, index);
    loading_bytecode = previous;
    Value function = vm_top(L)[-1];
    if (!status && function.tag == 8 && donor_mask_ptr)
        prepare_proto(*(uintptr_t*)(function.value + 0x18), 0);
    if (env) {
        vm_top(L)[-2] = function;
        *(Value**)((uint8_t*)L + L_TOP) -= 1;
    }
    return status;
}
static int get_identity(void* L) {
    double n = *(uint32_t*)identity_context();
    Value v = {.tag=3}; memcpy(&v.value, &n, sizeof(n)); push(L, v);
    return 1;
}
static int workspace_path(Value* value, char* path, size_t capacity) {
    if (value->tag != 6 || !value->value) return 0;
    uint32_t length = *(uint32_t*)(value->value + 0x14);
    const char* name = (const char*)(value->value + 0x18);
    if (!length || length > 200 || name[0] == '/' || memchr(name, '\\', length) ||
        memchr(name, '\0', length)) return 0;
    size_t segment = 0;
    for (uint32_t i = 0; i <= length; ++i) {
        if (i != length && name[i] != '/') continue;
        size_t width = i - segment;
        if (!width || (width == 1 && name[segment] == '.') ||
            (width == 2 && name[segment] == '.' && name[segment + 1] == '.')) return 0;
        segment = i + 1;
    }
    return snprintf(path, capacity, WORKSPACE "/%.*s", (int)length, name) < (int)capacity;
}
static int open_workspace_file(const char* path, int writing) {
    int dir = open(WORKSPACE, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    char relative[224];
    snprintf(relative, sizeof(relative), "%s", path + sizeof(WORKSPACE));
    char* segment = relative;
    char* slash;
    while ((slash = strchr(segment, '/')) != NULL) {
        *slash = '\0';
        if (writing && mkdirat(dir, segment, 0777) && errno != EEXIST) {
            int saved = errno; close(dir); errno = saved; return -1;
        }
        int next = openat(dir, segment, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int saved = errno;
        close(dir);
        if (next < 0) { errno = saved; return -1; }
        dir = next;
        segment = slash + 1;
    }
    int fd = openat(dir, segment, O_CLOEXEC | O_NOFOLLOW |
                    (writing ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY), 0666);
    int saved = errno;
    close(dir);
    if (fd < 0) errno = saved;
    return fd;
}
static int fs_error(void* L, const char* operation) {
    char reason[180];
    snprintf(reason, sizeof(reason), "%s: %s", operation, strerror(errno));
    push(L, (Value){0});
    push_text(L, reason);
    return 2;
}
static int x86_readfile(void* L) {
    Value* args = vm_base(L);
    char path[320];
    if (vm_top(L) <= args || !workspace_path(args, path, sizeof(path))) {
        push(L, (Value){0}); push_text(L, "readfile: invalid relative path"); return 2;
    }
    int fd = open_workspace_file(path, 0);
    if (fd < 0) return fs_error(L, "readfile");
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > 8 * 1024 * 1024) {
        close(fd); push(L, (Value){0}); push_text(L, "readfile: not a regular file under 8 MiB"); return 2;
    }
    size_t length = (size_t)info.st_size, at = 0;
    char* data = malloc(length ? length : 1);
    if (!data) { close(fd); errno = ENOMEM; return fs_error(L, "readfile"); }
    while (at < length) {
        ssize_t count = read(fd, data + at, length - at);
        if (count <= 0) { int saved = count < 0 ? errno : EIO; free(data); close(fd); errno = saved; return fs_error(L, "readfile"); }
        at += (size_t)count;
    }
    close(fd);
    void* string = g_engine.new_string(L, data, length);
    free(data);
    push(L, (Value){.value=(uintptr_t)string, .tag=6});
    return 1;
}
static int x86_writefile(void* L) {
    Value* args = vm_base(L);
    char path[320];
    if (vm_top(L) - args < 2 || !workspace_path(args, path, sizeof(path)) ||
        args[1].tag != 6 || !args[1].value) {
        push(L, (Value){0}); push_text(L, "writefile: expected a relative path and text"); return 2;
    }
    uint32_t length = *(uint32_t*)(args[1].value + 0x14);
    if (length > 8u * 1024u * 1024u) {
        push(L, (Value){0}); push_text(L, "writefile: file exceeds 8 MiB"); return 2;
    }
    int fd = open_workspace_file(path, 1);
    if (fd < 0) return fs_error(L, "writefile");
    const char* data = (const char*)(args[1].value + 0x18);
    size_t at = 0;
    while (at < length) {
        ssize_t count = write(fd, data + at, length - at);
        if (count <= 0) { int saved = count < 0 ? errno : EIO; close(fd); errno = saved; return fs_error(L, "writefile"); }
        at += (size_t)count;
    }
    if (close(fd)) return fs_error(L, "writefile");
    push(L, (Value){.value=1, .tag=1});
    return 1;
}
static int compile_source(void* L) {
    Value* args = vm_base(L);
    int argc = (int)(vm_top(L) - args);
    if (!argc || args[0].tag != 6) {
        push(L, (Value){0}); push_text(L, "loadstring expects a source string"); return 2;
    }
    uint32_t length = *(uint32_t*)(args[0].value + 0x14);
    if (length > 8u*1024u*1024u) {
        push(L, (Value){0}); push_text(L, "source exceeds 8 MiB"); return 2;
    }
    uintptr_t env = argc > 2 && args[2].tag == 7 ? args[2].value : donor_env;
    char name[100] = "=loadstring";
    if (argc > 1 && args[1].tag == 6) {
        size_t n = *(uint32_t*)(args[1].value + 0x14);
        if (n >= sizeof(name)) n = sizeof(name)-1;
        memcpy(name, (void*)(args[1].value + 0x18), n); name[n] = 0;
    }
    size_t size = 0;
    char* bytes = compile_encoded((const char*)(args[0].value + 0x18), length, &size);
    if (!bytes || !size || !bytes[0]) {
        push(L, (Value){0});
        if (bytes && size > 1) {
            void* error = g_engine.new_string(L, bytes + 1, size - 1);
            push(L, (Value){.value=(uintptr_t)error, .tag=6});
        } else push_text(L, "compiler failed");
        free(bytes); return 2;
    }
    int status = load_chunk(L, name, (uint8_t*)bytes, size, env);
    free(bytes);
    if (!status) return 1;
    Value error = vm_top(L)[-1];
    vm_top(L)[-1] = (Value){0}; push(L, error); return 2;
}
static int script_done(void* L) {
    uintptr_t ci = *(uintptr_t*)((uint8_t*)L + L_CI);
    Value* function = *(Value**)(ci + 8);
    Value* up = (Value*)(function->value + 0x30);
    double number = -1; memcpy(&number, &up->value, sizeof(number));
    int index = (int)number;
    if (up->tag != 3 || index < 0 || index >= 32 || !jobs[index].active) return 0;
    Value* args = vm_base(L);
    int success = args[0].tag == 1 && (uint32_t)args[0].value != 0;
    char path[256]; snprintf(path, sizeof(path), OUT_DIR "/%s.cmd.log", jobs[index].token);
    jobs[index].active = 0;
    unsigned active = 0;
    for (unsigned i = 0; i < 32; ++i) active += jobs[i].active != 0;
    FILE* file = fopen(path, "a");
    if (file) {
        char text[512] = "result none";
        if (vm_top(L) > args + 1) describe_value(args + 1, text, sizeof(text));
        fprintf(file, "%s%s\nexecutor active jobs=%u\nscript completed status=%d\n",
                success ? "" : "script error: ", text, active, success ? 0 : 2);
        fclose(file);
    }
    return 0;
}
static void run_scheduled(void* L, void* ud) {
    struct command_context* ctx = ud;
    set_identity(L, ctx);
    size_t size = 0;
    char* bytes = compile_encoded(scheduler_source, strlen(scheduler_source), &size);
    if (!bytes) { ctx->load_status = 1; return; }
    ctx->load_status = load_chunk(L, "=decode.scheduler", (uint8_t*)bytes, size, donor_env);
    free(bytes);
    if (ctx->load_status) return;
    g_engine.luaD_callint(L, vm_top(L)-1, 1, 0);
    ctx->load_status = load_chunk(L, "=decode", ctx->data, ctx->size, donor_env);
    if (ctx->load_status) return;
    double number = ctx->job; Value up = {.tag=3}; memcpy(&up.value, &number, sizeof(number));
    push(L, up);
    g_engine.lua_pushcclosurek(L, script_done, "decode.done", 1, NULL);
    g_engine.lua_pushcclosurek(L, get_identity, "getthreadidentity", 0, NULL);
    g_engine.lua_pushcclosurek(L, compile_source, "decode.compile", 0, NULL);
    g_engine.lua_pushcclosurek(L, x86_http_request_start, "decode.httpStart", 0, NULL);
    g_engine.lua_pushcclosurek(L, x86_http_request_poll, "decode.httpPoll", 0, NULL);
    g_engine.lua_pushcclosurek(L, x86_readfile, "decode.readfile", 0, NULL);
    g_engine.lua_pushcclosurek(L, x86_writefile, "decode.writefile", 0, NULL);
    g_engine.luaD_callint(L, vm_top(L)-9, 0, 0);
}

static void run_direct(void* L, void* ud) {
    struct command_context* ctx = ud;
    /* Script envs carry the identity/capabilities the engine checks; the env
       is passed to luau_load as the stack index of the pushed table. */
    int env_index = 0;
    if (donor_env) {
        uintptr_t top = *(uintptr_t*)((uint8_t*)L + L_TOP);
        uintptr_t base = *(uintptr_t*)((uint8_t*)L + 0x58);
        Value* slot = (Value*)top;
        slot->value = donor_env;
        slot->extra = 0;
        slot->tag = 7; /* LUA_TTABLE */
        *(uintptr_t*)((uint8_t*)L + L_TOP) = top + 16;
        env_index = (int)((top - base) / 16) + 1;
    }
    loading_bytecode = ctx->data;
    ctx->load_status = g_engine.luau_load(L, "=decode", (const char*)ctx->data,
                                          ctx->size, env_index);
    loading_bytecode = NULL;
    if (ctx->load_status != 0) {
        uintptr_t top = *(uintptr_t*)((uint8_t*)L + L_TOP);
        if (top >= ctx->original_top + 16) {
            Value* error = (Value*)(top - 16);
            if (error->tag == 6 && error->value) {
                describe_value(error, ctx->result, sizeof(ctx->result));
                ctx->has_result = 1;
            }
        }
    }
    if (ctx->load_status == 0) {
        uintptr_t top = *(uintptr_t*)((uint8_t*)L + L_TOP);
        Value* loaded = (Value*)(top - 16);
        uintptr_t proto = 0;
        if (ctx->token && loaded->tag == 8 && loaded->value && donor_mask_ptr &&
            probe_read(loaded->value + 0x18, &proto, sizeof(proto)) && proto)
            *(uintptr_t*)(proto + 0x38) = donor_mask_ptr;
#if BASALT_DEV_BRIDGE
        if (ctx->token) {
            uintptr_t context = 0, mask_ptr = 0;
            uint64_t thread_caps = 0, proto_caps = 0;
            probe_read((uintptr_t)L + 0x28, &context, sizeof(context));
            probe_read(context + 0x50, &thread_caps, sizeof(thread_caps));
            probe_read(proto + 0x38, &mask_ptr, sizeof(mask_ptr));
            probe_read(mask_ptr, &proto_caps, sizeof(proto_caps));
            report("loaded caps: thread=%p mask=0x%" PRIx64
                   " proto=%p mask_ptr=%p mask=0x%" PRIx64,
                   (void*)context, thread_caps, (void*)proto,
                   (void*)mask_ptr, proto_caps);
        }
#endif
        g_engine.luaD_callint(L, (void*)(top - 16), 1, 0);
        uintptr_t after = *(uintptr_t*)((uint8_t*)L + L_TOP);
        Value* slot = (Value*)(after - 16);
        describe_value(slot, ctx->result, sizeof(ctx->result));
        ctx->has_result = 1;
    }
    /* The coroutine has not started yet: rebalance the stack so the real
       resume path sees exactly the state it entered with. */
    *(uintptr_t*)((uint8_t*)L + L_TOP) = ctx->original_top;
}

static void begin_context(struct command_context* ctx, void* L) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->original_top = *(uintptr_t*)((uint8_t*)L + L_TOP);
    ctx->stack_base = *(uintptr_t*)((uint8_t*)L + 0x60);
}

/* Reading our own memory through the kernel reports an error for an unmapped
   candidate rather than faulting the engine thread on a speculative pointer. */
static int probe_read(uintptr_t address, void* out, size_t size) {
    if (address <= 0x10000) return 0;
    struct iovec local = { .iov_base = out, .iov_len = size };
    struct iovec remote = { .iov_base = (void*)address, .iov_len = size };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}

/* The callint callee is a live function mid-call. Validate its candidate
   environment with the `script` probe before adopting it. */
static int capture_donor(void* L, void* callee_slot) {
    if (donor_env || !callee_slot) return donor_env != 0;
    Value* func = (Value*)callee_slot;
    if (func->tag != 8 || !func->value) return 0;
    uint8_t* closure = (uint8_t*)func->value;
    /* 743 parity: the script environment lives at closure +0x10. A C closure
       stores its engine-text function pointer in the union, so require the
       candidate to be heap memory below the engine mapping. */
    uintptr_t candidate = *(uintptr_t*)(closure + 0x10);
    if (!candidate || candidate >= g_engine.base) return 0;
    uintptr_t proto = *(uintptr_t*)(closure + CLOSURE_ENV);
    if (!proto || proto >= g_engine.base) return 0;
    uintptr_t mask_ptr = 0;
    uint64_t mask = 0;
    probe_read(proto + 0x38, &mask_ptr, sizeof(mask_ptr));
    probe_read(mask_ptr, &mask, sizeof(mask));
    report("donor proto=%p mask_ptr=%p mask=0x%" PRIx64,
           (void*)proto, (void*)mask_ptr, mask);

    struct command_context ctx;
    begin_context(&ctx, L);
    ctx.data = donor_probe_bytecode;
    ctx.size = sizeof(donor_probe_bytecode);
    donor_env = candidate; /* adopted only if the probe passes */
    ptrdiff_t restore = (ptrdiff_t)(ctx.original_top - ctx.stack_base);
    int status = g_engine.luaD_pcall(L, run_direct, &ctx, restore, 0);
    int ok = status == 0 && ctx.load_status == 0 && ctx.has_result &&
             strcmp(ctx.result, "result 1") == 0;
    if (!ok) {
        donor_env = 0;
        donor_mask_ptr = 0;
        report("donor env rejected: status=%d load=%d result=\"%s\"",
               status, ctx.load_status, ctx.result);
        return 0;
    }
    donor_mask_ptr = mask_ptr;
    donor_global = *(uintptr_t*)((uint8_t*)L + L_GLOBAL);
    report("donor env validated: %p", (void*)donor_env);
    return 1;
}

static void process_command(void* L, const char* token) {
    char path[256];
    char stem[224];
    size_t token_length = strlen(token);
    if (token_length < 5 || token_length >= sizeof(stem) + 1) return;
    memcpy(stem, token, token_length - 4);
    stem[token_length - 4] = 0;
    snprintf(path, sizeof(path), IN_DIR "/%s", token);
    FILE* file = fopen(path, "rb");
    if (!file) return;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t* data = malloc(length > 0 ? (size_t)length : 1);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data); fclose(file); return;
    }
    fclose(file);
    unlink(path);

    char log[832];
    const char* body = (const char*)data;
    size_t size = (size_t)length;
    int mode = 0; // 0 direct, 1 inspect, 2 schedule
    if (length > 9 && memcmp(body, "EXEC_BIN\n", 9) == 0) { body += 9; size -= 9; }
    else if (length > 8 && memcmp(body, "INSPECT\n", 8) == 0) mode = 1;
    else if (length > 9 && memcmp(body, "SCHEDULE\n", 9) == 0) { mode = 2; body += 9; size -= 9; }
    if (mode == 1) {
        snprintf(log, sizeof(log), "load status=2\nrequest completed status=2\n");
        write_result(stem, log);
        free(data);
        return;
    }

    struct command_context ctx;
    begin_context(&ctx, L);
    ctx.data = (const uint8_t*)body;
    ctx.size = size;
    ctx.token = token;
    ctx.job = -1;
    if (mode == 2) {
        for (int i = 0; i < 32; ++i) if (!jobs[i].active) {
            ctx.job = i; jobs[i].active = 1;
            snprintf(jobs[i].token, sizeof(jobs[i].token), "%s", stem);
            break;
        }
        if (ctx.job < 0) {
            write_result(stem, "load status=2\nerror busy\nrequest completed status=2\n");
            free(data); return;
        }
    }
    ptrdiff_t restore = (ptrdiff_t)(ctx.original_top - ctx.stack_base);
    int status = g_engine.luaD_pcall(L, mode == 2 ? run_scheduled : run_direct, &ctx, restore, 0);
    restore_identity(&ctx);
    loading_bytecode = NULL;
    if (mode == 2 && (status || ctx.load_status)) {
        describe_value(vm_top(L)-1, ctx.result, sizeof(ctx.result));
        ctx.has_result = 1;
        jobs[ctx.job].active = 0;
    }
    if (ctx.load_status != 0) {
        snprintf(log, sizeof(log), "load status=%d\n%s\nrequest completed status=%d\n",
                 ctx.load_status, ctx.has_result ? ctx.result : "load error unavailable",
                 ctx.load_status);
    } else if (status != 0) {
        if (ctx.has_result) {
            snprintf(log, sizeof(log), "load status=0\nprotected execution status=%d\n%s\nrequest completed status=%d\n",
                     status, ctx.result, status);
        } else {
        char error_text[256] = "";
        for (unsigned i = 1; i <= 12 && !error_text[0]; ++i) {
            Value* slot = (Value*)(ctx.original_top + (size_t)i * 16);
            if (slot->tag != 6 || !slot->value) continue;
            const uint8_t* string = (const uint8_t*)slot->value;
            uint32_t length32;
            memcpy(&length32, string + 0x14, sizeof(length32));
            if (!length32 || length32 > 200) continue;
            memcpy(error_text, string + 0x18, length32);
            error_text[length32] = 0;
        }
        if (error_text[0])
            snprintf(log, sizeof(log), "load status=0\nprotected execution status=%d\nerror \"%s\"\nrequest completed status=%d\n",
                     status, error_text, status);
        else
            snprintf(log, sizeof(log), "load status=0\nprotected execution status=%d\nrequest completed status=%d\n",
                      status, status);
        }
    } else if (mode == 2) {
        snprintf(log, sizeof(log), "load status=0\nscript scheduled\nrequest completed status=0\n");
    } else {
        snprintf(log, sizeof(log), "load status=0\nprotected execution status=0\n%s\nrequest completed status=0\n",
                 ctx.has_result ? ctx.result : "result none");
    }
    write_result(stem, log);
    *(uintptr_t*)((uint8_t*)L + L_TOP) = *(uintptr_t*)((uint8_t*)L + 0x60) + restore;
    free(data);
    report("command %s done: status=%d", token, status);
}

static void bootstrap_autoexec(void* L) {
    if (autoexec_bootstrapped) return;
    DIR* dir = opendir(AUTOEXEC);
    if (!dir) return; // Retry after shared-storage access is granted.
    autoexec_bootstrapped = 1;
    struct dirent* entry;
    unsigned sequence = 0;
    while ((entry = readdir(dir)) && sequence < 32) {
        size_t length = strlen(entry->d_name);
        if (length > 200 || length < 5 || entry->d_name[0] == '.' ||
            (strcmp(entry->d_name + length - 4, ".lua") != 0 &&
             strcmp(entry->d_name + length - 5, ".luau") != 0)) continue;
        char path[320];
        snprintf(path, sizeof(path), AUTOEXEC "/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct stat info;
        if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            info.st_size > 1024 * 1024) { close(fd); continue; }
        size_t source_size = (size_t)info.st_size, at = 0;
        char* source = malloc(source_size);
        if (!source) { close(fd); break; }
        while (at < source_size) {
            ssize_t count = read(fd, source + at, source_size - at);
            if (count <= 0) break;
            at += (size_t)count;
        }
        close(fd);
        if (at != source_size) { free(source); continue; }
        char token[80];
        snprintf(token, sizeof(token), "autoexec-%02u", ++sequence);
        size_t bytecode_size = 0;
        char* bytecode = compile_encoded(source, source_size, &bytecode_size);
        free(source);
        if (!bytecode || !bytecode_size || !bytecode[0] || bytecode_size > 32768) {
            write_result(token, "autoexec compile failed or bytecode exceeds 32 KiB\nscript completed status=2\n");
            free(bytecode); continue;
        }
        int job = -1;
        for (int i = 0; i < 32; ++i) if (!jobs[i].active) { job = i; break; }
        if (job < 0) { free(bytecode); break; }
        jobs[job].active = 1;
        snprintf(jobs[job].token, sizeof(jobs[job].token), "%s", token);
        char log[300];
        snprintf(log, sizeof(log), "autoexec source=%s\n", entry->d_name);
        write_result(token, log);
        struct command_context ctx;
        begin_context(&ctx, L);
        ctx.data = (uint8_t*)bytecode;
        ctx.size = bytecode_size;
        ctx.job = job;
        ptrdiff_t restore = (ptrdiff_t)(ctx.original_top - ctx.stack_base);
        int status = g_engine.luaD_pcall(L, run_scheduled, &ctx, restore, 0);
        restore_identity(&ctx);
        loading_bytecode = NULL;
        *(uintptr_t*)((uint8_t*)L + L_TOP) = *(uintptr_t*)((uint8_t*)L + 0x60) + restore;
        free(bytecode);
        if (status || ctx.load_status) {
            jobs[job].active = 0;
            FILE* log_file;
            char result_path[256];
            snprintf(result_path, sizeof(result_path), OUT_DIR "/%s.cmd.log", token);
            log_file = fopen(result_path, "a");
            if (log_file) { fprintf(log_file, "autoexec load failed: protected=%d load=%d\nscript completed status=2\n", status, ctx.load_status); fclose(log_file); }
        }
    }
    closedir(dir);
}

static void scan_channel(void* L, void* callee_slot) {
    /* Joining another game keeps the process but tears down the old DataModel
       and its VM. Recapture the donor and run autoexec once in the new VM. */
    if (donor_env && *(uintptr_t*)((uint8_t*)L + L_GLOBAL) != donor_global) {
        if (!callee_slot) return;
        donor_env = 0;
        donor_mask_ptr = 0;
        donor_global = 0;
        autoexec_bootstrapped = 0;
        memset(jobs, 0, sizeof(jobs));
        report("VM changed: donor reset for recapture");
    }
    if (!donor_env && callee_slot) capture_donor(L, callee_slot);
    DIR* dir = opendir(IN_DIR);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".cmd") != 0) continue;
        if (entry->d_name[0] == '.') continue;
        /* A validated script env is the gate: it proves the calling context
           carries script identity, and chunks loaded into it inherit the
           script's capabilities. */
        if (!donor_env && !capture_donor(L, callee_slot)) break;
        if (*(uintptr_t*)((uint8_t*)L + L_GLOBAL) != donor_global) break;
        bootstrap_autoexec(L);
        process_command(L, entry->d_name);
    }
    closedir(dir);
    if (donor_env && !autoexec_bootstrapped && *(uintptr_t*)((uint8_t*)L + L_GLOBAL) == donor_global)
        bootstrap_autoexec(L);
}

static unsigned call_count;

static int x86_bytecode_gate(void* L, const char* name, const char* bytes,
                             size_t size, int env) {
    int status = original_bytecode_gate(L, name, bytes, size, env);
    if ((const uint8_t*)bytes == loading_bytecode) {
        report("bytecode gate: length=%zu result=%d prefix=%02x %02x", size,
               status, (unsigned char)bytes[0], (unsigned char)bytes[1]);
        if (status == 1 && size >= 0x118) return 0;
    }
    return status;
}

/* luaD_callint entry: fires mid-execution, where the calling thread's
   capabilities (script identity, UI capability) are live. This is where
   privileged commands actually succeed. */
void x86_callint_detour(void* L, void* func) {
    if (!channel_ready || __atomic_load_n(&dispatching, __ATOMIC_ACQUIRE)) return;
    ++call_count;
    if (call_count < 4096 || (call_count & 2047) != 0) return;
    __atomic_store_n(&dispatching, 1, __ATOMIC_RELEASE);
    scan_channel(L, func);
    __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
}

void x86_hook_detour(void* L, void* from) {
    (void)from;
    if (!channel_ready || __atomic_load_n(&dispatching, __ATOMIC_ACQUIRE)) return;
    ++resume_count;
    if (resume_count < 256 || (resume_count & 31) != 0) return;
    if (!donor_env) return; /* the callint path captures and validates the env */
    __atomic_store_n(&dispatching, 1, __ATOMIC_RELEASE);
    scan_channel(L, NULL);
    __atomic_store_n(&dispatching, 0, __ATOMIC_RELEASE);
}

int x86_executor_start(void) {
    mkdir(CHANNEL, 0777);
    mkdir(IN_DIR, 0777);
    mkdir(OUT_DIR, 0777);
    mkdir(WORKSPACE, 0777);
    mkdir(AUTOEXEC, 0777);
    unlink(LOG_PATH);
    FILE* session = fopen(SESSION, "w");
    if (session) {
        fprintf(session, "pid=%d\nprotocol=3\nloader=engine-native\nschedule=1\nvm=game\n", getpid());
        fclose(session);
    }
    if (hook_x86_inline(g_engine.resume_entry, (void*)x86_hook_detour, 16) != HOOK_OK) {
        report("x86 dispatch hook failed");
        return -1;
    }
    // Mid-execution dispatch: capabilities are live while a script call runs.
    extern void x86_callint_detour(void*, void*);
    /* luaD_callint: prologue ends mid-instruction at 16; steal 14 bytes. */
    if (hook_x86_inline((uint64_t)g_engine.luaD_callint, (void*)x86_callint_detour, 14) != HOOK_OK)
        report("x86 callint hook failed (resume path only)");
    if (hook_x86_replace(g_engine.base + 0x026EB9CE, (void*)x86_bytecode_gate, 15,
                         (void**)&original_bytecode_gate) != HOOK_OK)
        report("x86 bytecode gate hook failed");
    channel_ready = 1;
    report("x86 dispatch hook ready: engine=0x%llx resume=0x%llx",
           (unsigned long long)g_engine.base, (unsigned long long)g_engine.resume_entry);
    return 0;
}

#endif
