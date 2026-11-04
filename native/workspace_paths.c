#include "workspace_paths.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef BASALT_PRIVATE_WORKSPACE
#define BASALT_PRIVATE_WORKSPACE "/sdcard/Android/data/com.roblox.client/files/basalt/workspace"
#endif
#ifndef BASALT_PUBLIC_ROOT
#define BASALT_PUBLIC_ROOT "/storage/emulated/0/Basalt"
#endif
#define PUBLIC_WORKSPACE BASALT_PUBLIC_ROOT "/workspace"

static pthread_mutex_t path_mutex = PTHREAD_MUTEX_INITIALIZER;
static int public_ready;
static unsigned copy_serial;

static int directory(const char* path)
{
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int copy_file(int source, int destination, const char* name)
{
    struct stat existing;
    if (!fstatat(destination, name, &existing, AT_SYMLINK_NOFOLLOW))
    {
        if (S_ISREG(existing.st_mode)) return 0;
        errno = EEXIST; return -1;
    }
    if (errno != ENOENT) return -1;
    int input = openat(source, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) return -1;
    struct stat st;
    if (fstat(input, &st) || !S_ISREG(st.st_mode))
    { close(input); errno = EINVAL; return -1; }
    char temporary[96];
    snprintf(temporary, sizeof(temporary), ".basalt-migrate-%ld-%u", (long)getpid(), ++copy_serial);
    int output = openat(destination, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0)
    {
        int code = errno;
        close(input);
        errno = code;
        return -1;
    }
    char buffer[16384];
    int result = 0;
    for (;;)
    {
        ssize_t size = read(input, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) { result = -1; break; }
        if (!size) break;
        ssize_t at = 0;
        while (at < size)
        {
            ssize_t n = write(output, buffer + at, (size_t)(size - at));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = -1; break; }
            at += n;
        }
        if (result) break;
    }
    if (!result && fsync(output)) result = -1;
    int code = errno;
    close(input);
    if (close(output) && !result) { result = -1; code = errno; }
    // The kernel operation exists on older Android versions whose libc does
    // not export renameat2 (the libc symbol itself requires Android 11).
    if (!result && syscall(SYS_renameat2, destination, temporary, destination, name, RENAME_NOREPLACE))
    {
        code = errno;
        if (code != EEXIST || fstatat(destination, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode))
            result = -1;
    }
    unlinkat(destination, temporary, 0);
    errno = code;
    return result;
}

static int copy_directory(int source, int destination, unsigned depth)
{
    if (depth > 96) { errno = ELOOP; return -1; }
    int scan = dup(source);
    if (scan < 0) return -1;
    DIR* entries = fdopendir(scan);
    if (!entries) { close(scan); return -1; }
    int result = 0;
    struct dirent* entry;
    errno = 0;
    while ((entry = readdir(entries)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (S_ISREG(st.st_mode)) result = copy_file(source, destination, entry->d_name);
        else if (S_ISDIR(st.st_mode))
        {
            if (mkdirat(destination, entry->d_name, 0700) && errno != EEXIST) { result = -1; break; }
            int input = openat(source, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int output = openat(destination, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (input < 0 || output < 0) result = -1;
            else result = copy_directory(input, output, depth + 1);
            int code = errno;
            if (input >= 0) close(input);
            if (output >= 0) close(output);
            errno = code;
        }
        // Never follow links or copy special files out of the old workspace.
        if (result) break;
        errno = 0;
    }
    if (errno) result = -1;
    int code = errno;
    closedir(entries);
    errno = code;
    return result;
}

int workspace_prepare(void)
{
    if (__atomic_load_n(&public_ready, __ATOMIC_ACQUIRE)) return 1;
    pthread_mutex_lock(&path_mutex);
    if (!public_ready)
    {
        int root = directory(BASALT_PUBLIC_ROOT);
        int target = -1;
        if (root >= 0 && (!mkdirat(root, "workspace", 0700) || errno == EEXIST))
            target = openat(root, "workspace", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int source = -1;
        int success = 0;
        if (target >= 0)
        {
            source = open(BASALT_PRIVATE_WORKSPACE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            success = source >= 0 ? !copy_directory(source, target, 0) : errno == ENOENT;
        }
        if (source >= 0) close(source);
        if (target >= 0) close(target);
        if (root >= 0) close(root);
        if (success) __atomic_store_n(&public_ready, 1, __ATOMIC_RELEASE);
    }
    int ready = public_ready;
    pthread_mutex_unlock(&path_mutex);
    return ready;
}

const char* workspace_directory(void)
{
    workspace_prepare();
    if (__atomic_load_n(&public_ready, __ATOMIC_ACQUIRE)) return PUBLIC_WORKSPACE;
    // Workspace creation must not depend on the removed diagnostic channel.
    char parent[sizeof(BASALT_PRIVATE_WORKSPACE)];
    memcpy(parent, BASALT_PRIVATE_WORKSPACE, sizeof(parent));
    char* separator = strrchr(parent, '/');
    if (separator && separator != parent) {
        *separator = 0;
        int directory_fd = directory(parent);
        if (directory_fd >= 0) {
            mkdirat(directory_fd, separator + 1, 0700);
            close(directory_fd);
        }
    }
    return BASALT_PRIVATE_WORKSPACE;
}

int workspace_open_root(void)
{
    workspace_prepare();
    if (!__atomic_load_n(&public_ready, __ATOMIC_ACQUIRE))
        return open(BASALT_PRIVATE_WORKSPACE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int parent = open(BASALT_PUBLIC_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return -1;
    int root = openat(parent, "workspace", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int code = errno;
    close(parent);
    errno = code;
    return root;
}
