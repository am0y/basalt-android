#define BASALT_PRIVATE_WORKSPACE "private"
#define BASALT_PUBLIC_ROOT "public"
#include "../native/workspace_paths.c"
#include <assert.h>
#include <stdlib.h>

static void save(const char* path, const char* contents)
{
    FILE* file = fopen(path, "wb");
    assert(file);
    assert(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    assert(!fclose(file));
}
static void matches_file(const char* path, const char* expected)
{
    char bytes[256] = {0};
    FILE* file = fopen(path, "rb"); assert(file);
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    assert(size == strlen(expected) && !memcmp(bytes, expected, size));
    fclose(file);
}
int main(void)
{
    char temporary[] = "/tmp/opencode/basalt-workspace-XXXXXX";
    assert(mkdtemp(temporary) && !chdir(temporary));
    assert(!mkdir("private", 0700));
    assert(!mkdir("private/nested", 0700));
    save("private/draft.luau", "print('preserved')");
    save("private/nested/data.txt", "nested bytes");
    save("private/kept.txt", "old private copy");
    save("outside.txt", "outside workspace");
    assert(!symlink("../outside.txt", "private/link"));
    save("public", "permission/path unavailable");
    assert(!workspace_prepare());
    assert(!strcmp(workspace_directory(), "private"));
    assert(!unlink("public"));
    assert(!mkdir("public", 0700));
    assert(!mkdir("public/workspace", 0700));
    save("public/workspace/kept.txt", "keep newer public copy");
    assert(workspace_prepare());
    assert(!strcmp(workspace_directory(), "public/workspace"));
    matches_file("public/workspace/draft.luau", "print('preserved')");
    matches_file("public/workspace/nested/data.txt", "nested bytes");
    matches_file("public/workspace/kept.txt", "keep newer public copy");
    matches_file("private/kept.txt", "old private copy");
    matches_file("private/draft.luau", "print('preserved')");
    assert(access("public/workspace/link", F_OK) && errno == ENOENT);
    int root = workspace_open_root(); assert(root >= 0); close(root);
    assert(!rename("public", "moved-public"));
    assert(!symlink("moved-public", "public"));
    assert(workspace_open_root() < 0);
    __atomic_store_n(&public_ready, 0, __ATOMIC_RELEASE);
    assert(!workspace_prepare());
    puts("Workspace migration: nested files, conflict preservation, fallback and symlink checks OK");
    return 0;
}
