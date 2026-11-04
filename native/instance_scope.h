#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*instance_scope_reader)(uintptr_t, void*, size_t);

// Pinned ARM64 Instance layout, verified against temporary Folder/Actor pairs.
// Return -1 for an unreadable/cyclic chain; never treat that as a main VM.
static inline uintptr_t instance_actor_owner(uintptr_t instance, instance_scope_reader read)
{
    if (!instance) return UINTPTR_MAX;
    for (unsigned depth = 0; instance && depth < 256; ++depth)
    {
        uintptr_t descriptor = 0, name = 0, parent = 0;
        uint8_t text[24];
        if (!read(instance + 0x18, &descriptor, 8) || !descriptor ||
            !read(descriptor + 8, &name, 8) || !name || !read(name, text, sizeof(text)))
            return UINTPTR_MAX;
        if (!(text[0] & 1) && text[0] >> 1 == 5 && !memcmp(text + 1, "Actor", 5))
            return instance;
        if (!read(instance + 0x68, &parent, 8)) return UINTPTR_MAX;
        if (!parent) return 0;
        instance = parent;
    }
    return UINTPTR_MAX;
}
static inline int instance_actor_scope(uintptr_t instance, instance_scope_reader read)
{
    uintptr_t owner=instance_actor_owner(instance,read);
    return owner==UINTPTR_MAX ? -1 : owner ? 1 : 0;
}
