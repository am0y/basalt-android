#pragma once
#include <stddef.h>
#include <string.h>

static inline int workspace_write_extension_allowed(const char* path)
{
    static const char* const blocked[] = {
        ".exe", ".com", ".scr", ".pif", ".cpl", ".msc", ".bat", ".cmd", ".ps1", ".psd1", ".vbs", ".vbe",
        ".js", ".jse", ".wsf", ".wsh", ".hta", ".scf", ".lnk", ".zip", ".rar", ".7z", ".cab", ".iso", ".img",
        ".xml", ".msi", ".msp", ".reg", ".inf", ".url"
    };
    const char* leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    size_t length = strlen(leaf);
    while (length && (leaf[length - 1] == ' ' || leaf[length - 1] == '.')) --length;
    size_t dot = length;
    while (dot && leaf[dot - 1] != '.') --dot;
    if (!dot) return 1;
    --dot;
    for (size_t i = 0; i < sizeof(blocked) / sizeof(*blocked); ++i)
    {
        if (strlen(blocked[i]) != length - dot) continue;
        size_t j = 0;
        for (; j < length - dot; ++j)
        {
            unsigned char c = leaf[dot + j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)blocked[i][j]) break;
        }
        if (j == length - dot) return 0;
    }
    return 1;
}

static inline int workspace_normalize_path(const char* input, size_t length, char* output, size_t capacity)
{
    if (!length || !capacity || input[0] == '/' || input[0] == '\\') return 0;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)input[i] < 32 || input[i] == ':') return 0;
    size_t used = 0, start = 0;
    for (size_t i = 0; i <= length; ++i)
    {
        if (i < length && input[i] != '/' && input[i] != '\\') continue;
        size_t count = i - start;
        if (count == 2 && input[start] == '.' && input[start + 1] == '.')
        {
            if (!used) return 0;
            while (used && output[used - 1] != '/') --used;
            if (used) --used;
        }
        else if (count && !(count == 1 && input[start] == '.'))
        {
            if (used + !!used + count >= capacity) return 0;
            if (used) output[used++] = '/';
            memcpy(output + used, input + start, count);
            used += count;
        }
        start = i + 1;
    }
    output[used] = 0;
    return used != 0;
}
