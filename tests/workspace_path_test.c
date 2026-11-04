#include <assert.h>
#include <stdio.h>
#include "../native/workspace_path.h"

int main(void)
{
    const char* inputs[][2] = {
        {"folder/file.txt", "folder/file.txt"},
        {"unused/../file.txt", "file.txt"},
        {"a/b/../../file.txt", "file.txt"},
        {"a\\b\\file.txt", "a/b/file.txt"},
        {"./a//b/./../c", "a/c"},
        {"a/b/", "a/b"},
    };
    char result[192];
    for (size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i)
    {
        assert(workspace_normalize_path(inputs[i][0],strlen(inputs[i][0]),result,sizeof(result)));
        assert(!strcmp(result,inputs[i][1]));
    }
    const char* invalid[] = {"", "/outside", "\\outside", "C:\\outside", "../outside", "a/../../outside", "a/..", "a\nb"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i)
        assert(!workspace_normalize_path(invalid[i],strlen(invalid[i]),result,sizeof(result)));
    assert(!workspace_normalize_path("a\0b",3,result,sizeof(result)));
    assert(!workspace_normalize_path("abcd",4,result,4));
    assert(workspace_normalize_path("abcd",4,result,5));
    assert(!workspace_write_extension_allowed("folder/file.EXE"));
    assert(!workspace_write_extension_allowed("folder/file.exe. "));
    assert(!workspace_write_extension_allowed(".cmd"));
    assert(workspace_write_extension_allowed("folder.exe/file.luau"));
    assert(workspace_write_extension_allowed("file.exe.txt"));
    assert(workspace_write_extension_allowed("draft.luau"));
    puts("WORKSPACE_PATH_NORMALIZATION_OK");
}
