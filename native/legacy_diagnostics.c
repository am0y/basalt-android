#include "legacy_diagnostics.h"
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void cleanup_legacy_diagnostics(const char* root)
{
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) return;
    const char* names[] = {"in", "out"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        int fd = openat(parent, names[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        DIR* entries = fdopendir(fd);
        if (!entries) { close(fd); continue; }
        struct dirent* entry;
        while ((entry = readdir(entries)))
        {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            struct stat info;
            if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) continue;
            if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) unlinkat(fd, entry->d_name, 0);
        }
        closedir(entries);
        unlinkat(parent, names[i], AT_REMOVEDIR);
    }
    close(parent);
}
