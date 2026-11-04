#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* These rules apply only to the pinned engine's import table. */
static inline int protection_hide_open(const char* path)
{
    if (!path || strstr(path, "proc/self") || strstr(path, "dev/urandom") ||
        strstr(path, "proc/cpuinfo")) return 0;
    static const char* const indicators[] = {
        "apex/com.android.art/bin/daemonsu", "system/bin", "system/xbin", "sbin/su",
        "su/bin/su", "data/local/tmp/su", "data/local/su", "data/local/xbin/su",
        "su/bin/daemonsu", "product/bin/su", "system/framework", "system/sd/xbin/su",
        "system/bin/.ext/su", "system/usr/we-need-root/su", "cache/su", "data/su",
        "dev/su", "system/bin/failsafe/su", "data/adb/su/bin/su", "dev/socket/genyd",
        "dev/socket/baseband_genyd", "dev/socket/qemud", "system/app/Superuser",
        "system/app/Kinguser.apk", "system/app/SuperSU.apk", "system/app/KingoUser.apk",
        "data/app/com.topjohnwu.magisk", "data/app/com.kingroot.kinguser",
        "data/app/com.koushikdutta.superuser", "data/app/eu.chainfire.supersu",
        "data/dalvik", "sbin/magisk", "sbin/.magisk", "data/adb", "cache/magisk.log",
        "mnt/tirerack/magisk", "root/magisk", "XposedBridge", "libxposed_art",
        "MagiskManager", "com.topjohnwu.magisk", "product/bin/amphoras",
        "product/bin/daemonsu", "apex/com.android.runtime", "apex/com.android.art",
        "system_ext/bin/su", "system_ext/bin/amphoras", "system_ext/bin/daemonsu",
        "odm/bin", "vendor/bin", "vendor/xbin", "system/bin/venomsu",
        "system/bin/.ext/.su", "system/etc/init.d", "system/usr/share/superuser",
        "system/.tor", ".subackup", "su.d", "fstab.andy", "ueventd.andy.rc",
        "fstab.nox", "init.nox.rc", "ueventd.nox.rc", "dev/qemu_pipe",
        "ueventd.android_x86.rc", "x86.prop", "ueventd.ttVM_x86.rc",
        "init.ttVM_x86.rc", "fstab.ttVM_x86", "fstab.vbox86", "init.vbox86.rc",
        "ueventd.vbox86.rc",
    };
    for (size_t i = 0; i < sizeof(indicators)/sizeof(*indicators); ++i)
        if (strstr(path, indicators[i])) return 1;
    return 0;
}

static inline int protection_hide_maps(const char* path)
{
    if (!path || strncmp(path, "/proc/", 6)) return 0;
    const char* process = path + 6;
    const char* slash = strchr(process, '/');
    if (!slash) return 0;
    size_t n = (size_t)(slash - process);
    if (n != 4 || memcmp(process, "self", 4)) {
        if (!n) return 0;
        for (size_t i = 0; i < n; ++i)
            if (process[i] < '0' || process[i] > '9') return 0;
    }
    return !strcmp(slash, "/maps") || !strcmp(slash, "/smaps") ||
           !strcmp(slash, "/smaps_rollup");
}

static inline const char* protection_property(const char* name)
{
    if (!name) return NULL;
    static const struct { const char* name; const char* value; } properties[] = {
        {"ro.debuggable", "0"}, {"ro.adb.secure", "1"}, {"ro.secure", "1"},
        {"ro.warranty_bit", "0"}, {"ro.boot.warranty_bit", "0"},
        {"ro.boot.veritymode", "enforcing"}, {"ro.boot.flash.locked", "1"},
        {"ro.bootmode", "normal"}, {"init.svc.qemud", ""}, {"init.svc.qemu-props", ""},
    };
    for (size_t i = 0; i < sizeof(properties)/sizeof(*properties); ++i)
        if (!strcmp(name, properties[i].name)) return properties[i].value;
    return NULL;
}

enum protection_message { PROTECTION_CHARACTER, PROTECTION_STATUS, PROTECTION_PING };

/* Only edit the bytes appended by this serializer invocation. */
static inline int protection_clear_status(unsigned char* data, size_t length,
                                         size_t before, enum protection_message kind)
{
    if (!data || before > length || length > 16u*1024u*1024u) return 0;
    size_t remaining = length - before;
    size_t offset;
    if (kind == PROTECTION_CHARACTER) offset = 1;
    else if (kind == PROTECTION_STATUS) offset = 0x25;
    else if (kind == PROTECTION_PING) {
        if (remaining < 2) return 0;
        unsigned flags = data[before + 1];
        /* The optional 12-byte statistics block is selected by bits 1+2.
           Bits 3/4 add later fields and do not change this field's offset. */
        if ((flags & ~0x1eu) || ((flags & 6u) != 0 && (flags & 6u) != 6u)) return 0;
        offset = (flags & 6u) ? 0x1a : 0x0e;
    } else return 0;
    if (offset > remaining || remaining - offset < sizeof(uint32_t)) return 0;
    memset(data + before + offset, 0, sizeof(uint32_t));
    return 1;
}
