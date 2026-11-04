#include "runtime_protection.h"
#include "protection_policy.h"
#include "engine.h"
#include "hook.h"
#include "build_config.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

static atomic_uint installed;
static int (*engine_open)(const char*, int);
static FILE* (*engine_fopen)(const char*, const char*);
static char* (*engine_getenv)(const char*);
static int (*engine_property)(const char*, char*);

static int filtered_open(const char* path, int flags)
{
    if (protection_hide_open(path)) { errno = ENOENT; return -1; }
    return engine_open(path, flags);
}

static FILE* filtered_fopen(const char* path, const char* mode)
{
    if (protection_hide_maps(path)) { errno = ENFILE; return NULL; }
    return engine_fopen(path, mode);
}

static char* filtered_getenv(const char* name)
{
    return name && !strcmp(name, "PATH") ? NULL : engine_getenv(name);
}

static int filtered_property(const char* name, char* value)
{
    const char* replacement = protection_property(name);
    if (!replacement || !value) return engine_property(name, value);
    size_t size = strlen(replacement);
    if (size >= PROP_VALUE_MAX) return engine_property(name, value);
    memcpy(value, replacement, size + 1);
    return (int)size;
}

enum { STATUS, CHARACTER, PING, CONSTRUCTOR, FLAGS, HOOK_COUNT };
static struct inline_hook hooks[HOOK_COUNT];
typedef uintptr_t (*serializer)(void*, void*);

static uint32_t writer_position(const void* writer)
{
    uint32_t position;
    memcpy(&position, (const char*)writer + 0x30, sizeof(position));
    return position;
}

static uintptr_t serialize_status(void* self, void* writer, unsigned which,
                                  enum protection_message kind)
{
    serializer original = (serializer)hooks[which].trampoline;
    if (!writer) return original(self, writer);
    uint32_t before = writer_position(writer);
    uintptr_t result = original(self, writer);
    if (result) {
        uint32_t after = writer_position(writer);
        unsigned char* data;
        memcpy(&data, (const char*)writer + 0x10, sizeof(data));
        protection_clear_status(data, after, before, kind);
    }
    return result;
}

static uintptr_t status_message(void* self, void* writer)
{ return serialize_status(self, writer, STATUS, PROTECTION_STATUS); }
static uintptr_t character_message(void* self, void* writer)
{ return serialize_status(self, writer, CHARACTER, PROTECTION_CHARACTER); }
static uintptr_t ping_message(void* self, void* writer)
{ return serialize_status(self, writer, PING, PROTECTION_PING); }

static uintptr_t status_constructor(uintptr_t self, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                    uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    typedef uintptr_t (*constructor)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                     uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    uintptr_t result = ((constructor)hooks[CONSTRUCTOR].trampoline)(self,a1,a2,a3,a4,a5,a6,a7);
    if (self) {
        uint32_t value = 7;
        memcpy((void*)(self + 0x60), &value, sizeof(value));
    }
    return result;
}

static uint32_t status_flags(void* self)
{ (void)self; return 0; }

struct module { uintptr_t base; const ElfW(Phdr)* ph; size_t count; };

static int find_engine(struct dl_phdr_info* info, size_t size, void* context)
{
    (void)size;
    if ((uintptr_t)info->dlpi_addr != g_engine.base) return 0;
    *(struct module*)context = (struct module){g_engine.base, info->dlpi_phdr, info->dlpi_phnum};
    return 1;
}

static int contains(const struct module* m, uintptr_t address, size_t size, unsigned flags)
{
    if (address < m->base || size > UINTPTR_MAX - address) return 0;
    uintptr_t rva = address - m->base;
    for (size_t i = 0; i < m->count; ++i) {
        const ElfW(Phdr)* ph = m->ph + i;
        if (ph->p_type == PT_LOAD && (ph->p_flags & flags) == flags && rva >= ph->p_vaddr &&
            rva - ph->p_vaddr <= ph->p_memsz && size <= ph->p_memsz - (rva - ph->p_vaddr)) return 1;
    }
    return 0;
}

static int pinned_build(const struct module* m)
{
#if defined(BASALT_ARM_744) && BASALT_ARM_744
    static const unsigned char build_id[] = {
        0xcd,0x14,0x33,0x7f,0xf9,0x5a,0x49,0x77,0x63,0xbf,
        0x0f,0x2b,0x08,0xd2,0x35,0x93,0x14,0xbc,0x66,0x3b,
    };
#else
    static const unsigned char build_id[] = {
        0x4a,0x91,0x6f,0x58,0x38,0x6d,0x0c,0x32,0x9a,0x12,
        0x02,0xbf,0xc4,0xdf,0x0c,0x39,0x2e,0x86,0x94,0x87,
    };
#endif
    for (size_t i = 0; i < m->count; ++i) {
        const ElfW(Phdr)* ph = m->ph + i;
        if (ph->p_type != PT_NOTE || !contains(m, m->base + ph->p_vaddr, ph->p_memsz, PF_R)) continue;
        size_t offset = 0;
        while (ph->p_memsz - offset >= sizeof(ElfW(Nhdr))) {
            const unsigned char* p = (void*)(m->base + ph->p_vaddr + offset);
            ElfW(Nhdr) note; memcpy(&note, p, sizeof(note));
            size_t name_size = ((size_t)note.n_namesz + 3) & ~(size_t)3;
            size_t desc_size = ((size_t)note.n_descsz + 3) & ~(size_t)3;
            size_t remaining = ph->p_memsz - offset - sizeof(note);
            if (name_size > remaining || desc_size > remaining - name_size) return 0;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                note.n_descsz == sizeof(build_id) && !memcmp(p + sizeof(note), "GNU", 4))
                return !memcmp(p + sizeof(note) + name_size, build_id, sizeof(build_id));
            offset += sizeof(note) + name_size + desc_size;
        }
    }
    return 0;
}

static uintptr_t image_pointer(const struct module* m, uintptr_t value)
{
    if (contains(m, value, 1, PF_R)) return value;
    if (value <= UINTPTR_MAX - m->base && contains(m, m->base + value, 1, PF_R)) return m->base + value;
    return 0;
}

static uintptr_t* import_slot(const struct module* m, uintptr_t rva, const char* name)
{
    const ElfW(Dyn)* dynamic = NULL;
    size_t count = 0;
    for (size_t i = 0; i < m->count; ++i) if (m->ph[i].p_type == PT_DYNAMIC) {
        if (!contains(m, m->base+m->ph[i].p_vaddr, m->ph[i].p_memsz, PF_R)) return NULL;
        dynamic = (void*)(m->base + m->ph[i].p_vaddr);
        count = m->ph[i].p_memsz / sizeof(*dynamic);
    }
    uintptr_t strings = 0, symbols = 0, rela = 0;
    size_t string_size = 0, rela_size = 0, syment = 0;
    unsigned kind = 0;
    for (size_t i = 0; i < count && dynamic[i].d_tag != DT_NULL; ++i) {
        uintptr_t value = dynamic[i].d_un.d_val;
        switch (dynamic[i].d_tag) {
        case DT_STRTAB: strings = image_pointer(m,value); break;
        case DT_STRSZ: string_size = value; break;
        case DT_SYMTAB: symbols = image_pointer(m,value); break;
        case DT_SYMENT: syment = value; break;
        case DT_JMPREL: rela = image_pointer(m,value); break;
        case DT_PLTRELSZ: rela_size = value; break;
        case DT_PLTREL: kind = (unsigned)value; break;
        }
    }
    if (kind != DT_RELA || syment != sizeof(ElfW(Sym)) || !string_size ||
        !contains(m,strings,string_size,PF_R) || !contains(m,rela,rela_size,PF_R) ||
        rela_size % sizeof(ElfW(Rela)) || rva > UINTPTR_MAX - m->base ||
        !contains(m,m->base+rva,sizeof(uintptr_t),PF_R)) return NULL;
    const ElfW(Rela)* entries = (void*)rela;
    for (size_t i = 0; i < rela_size / sizeof(*entries); ++i) {
        if (entries[i].r_offset != rva || ELF64_R_TYPE(entries[i].r_info) != R_AARCH64_JUMP_SLOT) continue;
        size_t index = ELF64_R_SYM(entries[i].r_info);
        if (index > (UINTPTR_MAX-symbols)/sizeof(ElfW(Sym))) return NULL;
        uintptr_t address = symbols + index * sizeof(ElfW(Sym));
        if (!contains(m,address,sizeof(ElfW(Sym)),PF_R)) return NULL;
        const ElfW(Sym)* symbol = (void*)address;
        size_t length = strlen(name) + 1;
        if (symbol->st_name > string_size || length > string_size-symbol->st_name ||
            memcmp((void*)(strings+symbol->st_name),name,length)) return NULL;
        return (void*)(m->base+rva);
    }
    return NULL;
}

static struct {
    uintptr_t rva;
    const char* name;
    uintptr_t replacement;
    uintptr_t* slot;
    uintptr_t original;
} imports[] = {
#if defined(BASALT_ARM_744) && BASALT_ARM_744
    {0x06c0dd90,"__open_2",(uintptr_t)filtered_open,NULL,0},
    {0x06c0df68,"__system_property_get",(uintptr_t)filtered_property,NULL,0},
    {0x06c0dc48,"getenv",(uintptr_t)filtered_getenv,NULL,0},
    {0x06c0d988,"fopen",(uintptr_t)filtered_fopen,NULL,0},
#else
    {0x06a7ee20,"__open_2",(uintptr_t)filtered_open,NULL,0},
    {0x06a7eff8,"__system_property_get",(uintptr_t)filtered_property,NULL,0},
    {0x06a7ecc8,"getenv",(uintptr_t)filtered_getenv,NULL,0},
    {0x06a7ea20,"fopen",(uintptr_t)filtered_fopen,NULL,0},
#endif
};

static int write_imports(const struct module* m, int restore)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((size_t)page_size & ((size_t)page_size-1))) return -1;
    uintptr_t mask = (uintptr_t)page_size - 1;
    uintptr_t page = (uintptr_t)imports[0].slot & ~mask;
    /* All four verified slots occupy the same RELRO page on this build. */
    for (size_t i = 0; i < sizeof(imports)/sizeof(*imports); ++i)
        if (((uintptr_t)imports[i].slot & ~mask) != page) return -1;
    int relro = 0;
    for (size_t i = 0; i < m->count; ++i) {
        const ElfW(Phdr)* ph = m->ph+i;
        if (ph->p_type == PT_GNU_RELRO && page >= ((m->base+ph->p_vaddr)&~mask) &&
            page+(size_t)page_size <= ((m->base+ph->p_vaddr+ph->p_memsz+mask)&~mask)) relro = 1;
    }
    if (!relro || mprotect((void*)page,(size_t)page_size,PROT_READ|PROT_WRITE)) return -1;
    for (size_t i = 0; i < sizeof(imports)/sizeof(*imports); ++i)
        __atomic_store_n(imports[i].slot, restore ? imports[i].original : imports[i].replacement, __ATOMIC_RELEASE);
    if (!mprotect((void*)page,(size_t)page_size,PROT_READ)) return 0;
    if (!restore) {
        for (size_t i = 0; i < sizeof(imports)/sizeof(*imports); ++i)
            __atomic_store_n(imports[i].slot,imports[i].original,__ATOMIC_RELEASE);
        mprotect((void*)page,(size_t)page_size,PROT_READ);
    }
    return -1;
}

uint32_t runtime_protection_state(void)
{ return atomic_load_explicit(&installed,memory_order_acquire); }

#if BASALT_DEV_BRIDGE
int runtime_protection_probe(void)
{
    if (runtime_protection_state() != 0x1ffu) return 0;
    for (size_t i = 0; i < sizeof(imports)/sizeof(*imports); ++i)
        if (__atomic_load_n(imports[i].slot,__ATOMIC_ACQUIRE) != imports[i].replacement) return 0;
    char value[PROP_VALUE_MAX];
    int (*property)(const char*,char*) = (void*)*imports[1].slot;
    char* (*environment)(const char*) = (void*)*imports[2].slot;
    FILE* (*open_file)(const char*,const char*) = (void*)*imports[3].slot;
    int (*open_fd)(const char*,int) = (void*)*imports[0].slot;
    if (property("ro.debuggable",value) != 1 || strcmp(value,"0") || environment("PATH")) return 0;
    char expected[PROP_VALUE_MAX];
    int n = engine_property("ro.product.model",expected);
    if (property("ro.product.model",value) != n || strcmp(expected,value)) return 0;
    errno = 0;
    FILE* map = open_file("/proc/self/maps","r");
    if (map || errno != ENFILE) { if (map) fclose(map); return 0; }
    errno = 0;
    int fd = open_fd("/system/bin/su",O_RDONLY);
    if (fd >= 0 || errno != ENOENT) { if (fd >= 0) close(fd); return 0; }
    FILE* original = engine_fopen("/proc/self/maps","r");
    if (!original) return 0;
    fclose(original);
    return 1;
}
#endif

int runtime_protection_init(void)
{
    if (runtime_protection_state() == 0x1ffu) return 0;
    struct module module = {0};
    if (!g_engine.base || dl_iterate_phdr(find_engine,&module) != 1 || !pinned_build(&module)) {
        LOGE("runtime protection: unsupported engine build"); return -1;
    }
    static const struct { uintptr_t rva; uint32_t code[4]; void* replacement; } targets[] = {
#if defined(BASALT_ARM_744) && BASALT_ARM_744
        {0x05eed8e0,{0xD10143FF,0xA9017BFD,0xA9025FF8,0xA90357F6},status_message},
        {0x05dc3b34,{0xD10183FF,0xA9027BFD,0xF9001BF7,0xA90457F6},character_message},
        {0x05e7dbc8,{0xD10203FF,0xA9027BFD,0xF9001BFB,0xA90467FA},ping_message},
        {0x05e7d90c,{0xD101C3FF,0xFD0013E8,0xA9037BFD,0xA9045FF8},status_constructor},
        {0x057cc314,{0xD10103FF,0xA9027BFD,0xA9034FF4,0x910083FD},status_flags},
#else
        {0x05d7c19c,{0xD10143FF,0xA9017BFD,0xA9025FF8,0xA90357F6},status_message},
        {0x05c53ba8,{0xD10183FF,0xA9027BFD,0xF9001BF7,0xA90457F6},character_message},
        {0x05d0e3fc,{0xD10203FF,0xA9027BFD,0xF9001BFB,0xA90467FA},ping_message},
        {0x05d0e140,{0xD101C3FF,0xFD0013E8,0xA9037BFD,0xA9045FF8},status_constructor},
        {0x0565f804,{0xD10103FF,0xA9027BFD,0xA9034FF4,0x910083FD},status_flags},
#endif
    };
    _Static_assert(sizeof(targets)/sizeof(*targets) == HOOK_COUNT,"hook table order");
    for (size_t i = 0; i < sizeof(imports)/sizeof(*imports); ++i) {
        imports[i].slot = import_slot(&module,imports[i].rva,imports[i].name);
        if (!imports[i].slot) goto invalid;
        imports[i].original = __atomic_load_n(imports[i].slot,__ATOMIC_ACQUIRE);
        if (!imports[i].original || imports[i].original == imports[i].replacement) goto invalid;
    }
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        uintptr_t address = module.base+targets[i].rva;
        if (!contains(&module,address,16,PF_R|PF_X) ||
            hook_arm64_prepare(hooks+i,address,targets[i].replacement,targets[i].code) != HOOK_OK) goto invalid;
    }
    engine_open = (void*)imports[0].original;
    engine_property = (void*)imports[1].original;
    engine_getenv = (void*)imports[2].original;
    engine_fopen = (void*)imports[3].original;
    if (write_imports(&module,0)) goto invalid;
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        if (hook_arm64_activate(hooks+i) != HOOK_OK) {
            write_imports(&module,1); goto invalid;
        }
    }
    atomic_store_explicit(&installed,0x1ffu,memory_order_release);
    LOGI("runtime protection ready: 4 imports, 5 status hooks");
    return 0;
invalid:
    for (size_t i = HOOK_COUNT; i > 0; --i)
        if (hook_arm64_restore(hooks+i-1) != HOOK_OK)
            atomic_fetch_or_explicit(&installed,0x80000000u,memory_order_release);
    LOGE("runtime protection: validation or installation failed");
    return -1;
}
