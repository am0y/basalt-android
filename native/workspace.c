#include "workspace.h"
#include "engine.h"
#include "sha384.h"
#include "workspace_paths.h"
#include "workspace_path.h"
#include "device_http.h"
#include "vm_layout.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORKSPACE workspace_directory()
#define FILE_IO_CHUNK (1024u * 1024u)
#define MAX_LUA_STRING ((size_t)1u << 30)

typedef struct { uint64_t value; uint32_t extra, tag; } TValue;
static TValue* base(void* L) { return *(TValue**)((uint8_t*)L + VM_L_API_BASE); }
static TValue* top(void* L) { return *(TValue**)((uint8_t*)L + VM_L_API_TOP); }

static void push(void* L, TValue value)
{
    if (!g_engine.check_stack(L, 1)) return;
    if (*((uint8_t*)L + VM_L_GC_FLAGS) & 4) g_engine.thread_barrier(L, L, (uint8_t*)L + VM_L_GC_LIST);
    *top(L) = value;
    *(TValue**)((uint8_t*)L + VM_L_API_TOP) = top(L) + 1;
}

static int failure(void* L, const char* message)
{
#if defined(BASALT_X86_ENGINE)
    push(L, (TValue){0});
    g_engine.lua_pushstring(L, message);
    return 2;
#else
    g_engine.lua_pushstring(L, message);
    g_engine.lua_error(L);
    return 0;
#endif
}

static int os_error(void* L)
{
    return failure(L, strerror(errno));
}

static int path_arg_at(void* L, int index, char* path, size_t capacity)
{
    if (g_engine.lua_gettop(L) < index || base(L)[index - 1].tag != 6) return 0;
    uintptr_t value = base(L)[index - 1].value;
    uint32_t length = *(uint32_t*)(value + 0x14);
    if (!length || length >= capacity) return 0;
    return workspace_normalize_path((const char*)(value + 0x18), length, path, capacity);
}
static int path_arg(void* L, char* path, size_t capacity)
{ return path_arg_at(L, 1, path, capacity); }

static int getworkspacepath(void* L)
{
    g_engine.lua_pushstring(L, WORKSPACE);
    return 1;
}

static int requestworkspaceaccess(void* L)
{
    char error[192] = {0};
    int state = workspace_prepare() ? 0 : device_request_storage_access(error, sizeof(error));
    if (state < 0) return failure(L, error);
    int ready = workspace_prepare();
    g_engine.lua_pushboolean(L, ready);
    g_engine.lua_pushstring(L, ready ? WORKSPACE : state == 1 ?
        "Enable file access in Android settings, then tap Storage again." :
        "Public storage is unavailable; the app-private workspace is still active.");
    return 2;
}

static int root_arg(void* L)
{
    if (g_engine.lua_gettop(L) < 1 || base(L)[0].tag != 6) return 0;
    uintptr_t string = base(L)[0].value;
    uint32_t size = *(uint32_t*)(string + 0x14);
    return !size || (size == 1 && *(char*)(string + 0x18) == '.');
}

static int path_parent_mode(const char* path, char* name, int create)
{
    int dirfd = workspace_open_root();
    if (dirfd < 0) return -1;
    const char* cursor = path;
    const char* slash;
    while ((slash = strchr(cursor, '/')))
    {
        char part[192];
        size_t n = (size_t)(slash - cursor);
        memcpy(part, cursor, n);
        part[n] = 0;
        int child = openat(dirfd, part, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (child < 0 && errno == ENOENT && create)
        {
            if (!mkdirat(dirfd, part, 0700) || errno == EEXIST)
                child = openat(dirfd, part, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        }
        int code = errno;
        close(dirfd);
        if (child < 0) { errno = code; return -1; }
        dirfd = child;
        cursor = slash + 1;
    }
    strcpy(name, cursor);
    return dirfd;
}
static int path_parent(const char* path, char* name) { return path_parent_mode(path, name, 0); }

static int file_write(void* L, int append)
{
    char path[192], name[192];
    if (!path_arg(L, path, sizeof(path)) || g_engine.lua_gettop(L) < 2 || base(L)[1].tag != 6)
        return failure(L, "writefile expects a workspace-relative path and a string");
    if (!workspace_write_extension_allowed(path))
        return failure(L, "file extension is not supported for workspace writes");
    uint32_t length = *(uint32_t*)(base(L)[1].value + 0x14);
    const char* data = (const char*)(base(L)[1].value + 0x18);
    int dirfd = path_parent_mode(path, name, 1);
    if (dirfd < 0) return os_error(L);
    int fd = openat(dirfd, name, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_CREAT |
                    (append ? O_APPEND : 0), 0600);
    int code = errno;
    close(dirfd);
    if (fd < 0) { errno = code; return os_error(L); }
    struct stat st;
    if (fstat(fd, &st))
    { code = errno; close(fd); errno = code; return os_error(L); }
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
    { close(fd); return failure(L, "writefile expects a regular file"); }
    if (append && (uint64_t)st.st_size > (uint64_t)INT64_MAX - length)
    { close(fd); return failure(L, "appended file exceeds the filesystem size range"); }
    if (!append && ftruncate(fd, 0))
    { code = errno; close(fd); errno = code; return os_error(L); }
    // Write straight from the rooted Luau string, without a second full-size copy.
    for (size_t at = 0; at < length;)
    {
        size_t count = length - at;
        if (count > FILE_IO_CHUNK) count = FILE_IO_CHUNK;
        ssize_t n = write(fd, data + at, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { code = n == 0 ? EIO : errno; close(fd); errno = code; return os_error(L); }
        at += n;
    }
    if (close(fd)) return os_error(L);
    return 0;
}

static int writefile(void* L) { return file_write(L, 0); }
static int appendfile(void* L) { return file_write(L, 1); }
static int renamefile(void* L)
{
    char from[192], to[192], source[192], target[192];
    if (!path_arg_at(L, 1, from, sizeof(from)) || !path_arg_at(L, 2, to, sizeof(to)))
        return failure(L, "renamefile expects two workspace-relative paths");
    if (!workspace_write_extension_allowed(to)) return failure(L, "file extension is not supported for workspace writes");
    int srcfd = path_parent(from, source);
    if (srcfd < 0) return os_error(L);
    int dstfd = path_parent_mode(to, target, 1);
    if (dstfd < 0) { int code=errno; close(srcfd); errno=code; return os_error(L); }
    struct stat current, replaced;
    int bad = fstatat(srcfd, source, &current, AT_SYMLINK_NOFOLLOW) || !S_ISREG(current.st_mode) ||
        (!fstatat(dstfd, target, &replaced, AT_SYMLINK_NOFOLLOW) && !S_ISREG(replaced.st_mode));
    if (bad) { close(srcfd); close(dstfd); return failure(L, "renamefile requires regular files"); }
    int result = renameat(srcfd, source, dstfd, target), code = errno;
    close(srcfd); close(dstfd);
    if (result) { errno=code; return os_error(L); }
    return 0;
}

struct file_contents { const char* bytes; size_t length; };
static void push_file_contents(void* L, void* context)
{
    struct file_contents* file = context;
    g_engine.lua_pushlstring(L, file->bytes, file->length);
}

static int readfile(void* L)
{
    char path[192], name[192];
    if (!path_arg(L, path, sizeof(path))) return failure(L, "readfile expects a workspace-relative path");
    int dirfd = path_parent(path, name);
    if (dirfd < 0) return os_error(L);
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int code = errno;
    close(dirfd);
    if (fd < 0) { errno = code; return os_error(L); }
    struct stat st;
    if (fstat(fd, &st))
    { code = errno; close(fd); errno = code; return os_error(L); }
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
    { close(fd); return failure(L, "readfile expects a regular file"); }
    if ((uint64_t)st.st_size > MAX_LUA_STRING)
    { close(fd); return failure(L, "readfile exceeds Luau's 1 GiB string limit; the file remains on disk"); }
    char* bytes = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!bytes) { close(fd); return failure(L, "out of memory"); }
    size_t at = 0;
    while (at < (size_t)st.st_size)
    {
        size_t count = (size_t)st.st_size - at;
        if (count > FILE_IO_CHUNK) count = FILE_IO_CHUNK;
        ssize_t n = read(fd, bytes + at, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(bytes); close(fd); return failure(L, "readfile did not return the entire file"); }
        at += n;
    }
    close(fd);
    struct file_contents file = {bytes, (size_t)st.st_size};
    ptrdiff_t saved = (uint8_t*)top(L) - *(uint8_t**)((uint8_t*)L + VM_L_API_STACK);
    int status = g_engine.luaD_pcall(L, push_file_contents, &file, saved, 0);
    free(bytes);
    if (status) { g_engine.lua_error(L); return 0; }
    return 1;
}

static int file_test(void* L, int directory)
{
    if (directory && root_arg(L))
    {
        int root = workspace_open_root();
        g_engine.lua_pushboolean(L, root >= 0);
        if (root >= 0) close(root);
        return 1;
    }
    char path[192], name[192];
    if (!path_arg(L, path, sizeof(path))) { g_engine.lua_pushboolean(L, 0); return 1; }
    int dirfd = path_parent(path, name);
    struct stat st;
    int ok = dirfd >= 0 && !fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) &&
             (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
    if (dirfd >= 0) close(dirfd);
    g_engine.lua_pushboolean(L, ok);
    return 1;
}
static int isfile(void* L) { return file_test(L, 0); }
static int isfolder(void* L) { return file_test(L, 1); }

static int remove_directory(int parent, const char* name)
{
    int fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    DIR* dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int result = 0;
    struct dirent* entry;
    errno = 0;
    while ((entry = readdir(dir)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            (S_ISDIR(st.st_mode) ? remove_directory(fd, entry->d_name) : unlinkat(fd, entry->d_name, 0)))
        { result = -1; break; }
        errno = 0;
    }
    if (errno) result = -1;
    int code = errno;
    closedir(dir);
    if (result) { errno = code; return -1; }
    return unlinkat(parent, name, AT_REMOVEDIR);
}

static int file_delete(void* L, int directory)
{
    char path[192], name[192];
    if (!path_arg(L, path, sizeof(path))) return failure(L, "invalid workspace-relative path");
    int dirfd = path_parent(path, name);
    if (dirfd < 0) return os_error(L);
    int result = directory ? remove_directory(dirfd, name) : unlinkat(dirfd, name, 0);
    int code = errno;
    close(dirfd);
    if (result && code != ENOENT) { errno = code; return os_error(L); }
    return 0;
}
static int delfile(void* L) { return file_delete(L, 0); }
static int delfolder(void* L) { return file_delete(L, 1); }

static int makefolder(void* L)
{
    char path[192];
    if (!path_arg(L, path, sizeof(path))) return failure(L, "invalid workspace-relative path");
    int fd = workspace_open_root();
    if (fd < 0) return os_error(L);
    char* cursor = path;
    while (cursor)
    {
        char* slash = strchr(cursor, '/');
        if (slash) *slash = 0;
        if (mkdirat(fd, cursor, 0700) && errno != EEXIST)
        { int code = errno; close(fd); errno = code; return os_error(L); }
        int next = openat(fd, cursor, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        int code = errno;
        close(fd);
        if (next < 0) { errno = code; return os_error(L); }
        fd = next;
        cursor = slash ? slash + 1 : NULL;
    }
    close(fd);
    return 0;
}

static int listfiles(void* L)
{
    char path[192], name[192];
    int fd;
    if (root_arg(L)) { path[0] = 0; fd = workspace_open_root(); }
    else
    {
        if (!path_arg(L, path, sizeof(path))) return failure(L, "invalid workspace-relative path");
        int parent = path_parent(path, name);
        if (parent < 0) return os_error(L);
        fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        close(parent);
    }
    if (fd < 0) return os_error(L);
    DIR* dir = fdopendir(fd);
    if (!dir) { close(fd); return os_error(L); }
    void* table = g_engine.new_table(L, 0, 8);
    push(L, (TValue){.value=(uintptr_t)table, .tag=7});
    unsigned index = 1;
    struct dirent* entry;
    while ((entry = readdir(dir)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char value[512];
        snprintf(value, sizeof(value), "%s%s%s", path, path[0] ? "/" : "", entry->d_name);
        g_engine.lua_pushstring(L, value);
        TValue string = top(L)[-1];
        TValue key = {.value=0, .tag=3};
        double number = (double)index++;
        memcpy(&key.value, &number, 8);
        TValue* slot = g_engine.table_set(L, table, &key);
        *slot = string;
        if ((*((uint8_t*)table + 1) & 4) && (*((uint8_t*)string.value + 1) & 3))
            g_engine.table_barrier(L, table, (void*)string.value);
        *(TValue**)((uint8_t*)L + VM_L_API_TOP) = top(L) - 1;
    }
    closedir(dir);
    return 1;
}
static int getcustomasset(void* L)
{
    char path[192];
    if (!path_arg(L, path, sizeof(path))) return failure(L, "expected a workspace-relative file path");
    int result = readfile(L);
    if (result != 1) return result;
    TValue content = top(L)[-1];
    uint32_t size = *(uint32_t*)(content.value + 0x14);
    const char* bytes = (const char*)(content.value + 0x18);
    Sha384 hash;
    char digest[97];
    sha384_init(&hash); sha384_update(&hash, bytes, size); sha384_finish(&hash, digest);
    const char* extension = strrchr(path, '.');
    if (!extension || strlen(extension) > 12 || strchr(extension, '/')) extension = ".bin";
    char filename[128], destination[4096], url[160];
    snprintf(filename, sizeof(filename), "%s%s", digest, extension);
#if defined(BASALT_X86_ENGINE)
    const char* directory = getenv("BASALT_X86_ASSET_DIR");
    if (!directory || directory[0] != '/') return failure(L, "Linux asset directory is unavailable");
#else
    const char* directory = "/data/data/com.roblox.client/app_assets/content/basalt";
#endif
    if (mkdir(directory, 0700) && errno != EEXIST) return os_error(L);
    if (snprintf(destination, sizeof(destination), "%s/%s", directory, filename) >= (int)sizeof(destination))
        return failure(L, "asset path exceeds filesystem limit");
    // Asset readers run asynchronously. Never truncate a content-addressed
    // file that a Sound or ImageLabel may already have open.
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s/.asset-XXXXXX", directory) >= (int)sizeof(temporary))
        return failure(L, "asset path exceeds filesystem limit");
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) return os_error(L);
    for (size_t at = 0; at < size;)
    {
        ssize_t n = write(fd, bytes + at, size - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int code = errno; close(fd); unlink(temporary); errno = code; return os_error(L); }
        at += n;
    }
    if (close(fd) || rename(temporary, destination))
    { int code = errno; unlink(temporary); errno = code; return os_error(L); }
    snprintf(url, sizeof(url), "rbxasset://basalt/%s", filename);
    g_engine.lua_pushstring(L, url);
    return 1;
}

static void register_one(void* L, void* env, const char* name, int (*func)(void*))
{
    g_engine.lua_pushcclosurek(L, func, name, 0, NULL);
    TValue key = {.value=(uintptr_t)g_engine.new_string(L, name, strlen(name)), .tag=6};
    TValue closure = top(L)[-1];
    TValue* slot = g_engine.table_set(L, env, &key);
    *slot = closure;
    if ((*((uint8_t*)env + 1) & 4) && (*((uint8_t*)closure.value + 1) & 3))
        g_engine.table_barrier(L, env, (void*)closure.value);
    *(TValue**)((uint8_t*)L + VM_L_API_TOP) = top(L) - 1;
}

void register_workspace(void* L, void* env)
{
    mkdir(WORKSPACE, 0700);
    register_one(L, env, "getworkspacepath", getworkspacepath);
    register_one(L, env, "requestworkspaceaccess", requestworkspaceaccess);
    register_one(L, env, "writefile", writefile);
    register_one(L, env, "appendfile", appendfile);
    register_one(L, env, "renamefile", renamefile);
    register_one(L, env, "readfile", readfile);
    register_one(L, env, "isfile", isfile);
    register_one(L, env, "isfolder", isfolder);
    register_one(L, env, "makefolder", makefolder);
    register_one(L, env, "delfile", delfile);
    register_one(L, env, "delfolder", delfolder);
    register_one(L, env, "listfiles", listfiles);
    register_one(L, env, "getcustomasset", getcustomasset);
}
