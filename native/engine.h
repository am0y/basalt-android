#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*protected_callback)(void* L, void* context);

struct engine {
    uintptr_t base;
    uintptr_t text_start;
    size_t text_size;
    uintptr_t resume_entry;

    int (*luau_load)(void* L, const char* chunkname, const char* data, size_t size, int env);
    int (*luaD_pcall)(void* L, protected_callback callback, void* context, ptrdiff_t oldtop, ptrdiff_t errfunc);
    void (*luaD_callint)(void* L, void* func, int nresults, int preparereentry);
    int (*lua_gettop)(void* L);
    void (*lua_setreadonly)(void* L, int index, int enabled);
    void (*lua_pushcclosurek)(void* L, int (*callback)(void* L), const char* name, int nup, void* continuation);
    void* (*lua_newthread)(void* L);
    uint64_t (*effective_capabilities)(void* L);
    void (*lua_pushstring)(void* L, const char* text);
    void* (*vm_alloc)(void* L, size_t bytes, uint8_t memcat);
    void* (*new_proto)(void* L);
    void* (*new_lclosure)(void* L, int nups, void* env, void* proto);
    void* (*new_string)(void* L, const char* text, size_t length);
    void* (*new_table)(void* L, int array, int hash);
    void* (*table_set)(void* L, void* table, const void* key);
    int (*check_stack)(void* L, int count);
    void (*thread_barrier)(void* L, void* object, void* gclist);
    const void* (*table_get)(void* table, const void* key);
    void (*table_barrier)(void* L, void* table, void* value);
    void (*lua_pushvalue)(void* L, int index);
    void (*lua_pushlstring)(void* L, const char* bytes, size_t length);
    void (*lua_pushboolean)(void* L, int boolean);
    void (*lua_error)(void* L);
    void (*mark_object)(void* global, void* object);
    void (*push_instance_uncached)(void* L, const void* shared_pointer);
    void* (*security_context)(void* L);
    uint64_t (*identity_capabilities)(const void* identity);
    int (*register_userdata_direct)(void* L, int tag, void* get, void* set, void* namecall);
    void (*push_instance_cached)(void* L, const void* shared_pointer);
    void (*release_shared)(void* shared_pointer);
    uint32_t (*xxhash32)(const void* data, int size, uint32_t seed);
    size_t (*zstd_decompress)(void* output, size_t capacity, const void* input, size_t size);
    int (*push_variant)(const void* variant, void* L);
    void (*write_property)(const void* property_instance, void* L, void* security);
    void* (*new_userdata)(void* L, size_t bytes, int tag);
    void (*trigger_prompt)(void* prompt);
    void (*click_detector)(void* detector, float distance, void* player);
    void (*right_click_detector)(void* detector, float distance, void* player);
    void (*signal_player)(void* signal, const void* shared_player);
    void (*replicate_click)(void* descriptor, void* detector, void* shared_player, int event);
    void (*touch_part)(void* part, const void* other, int ended, int local_simulation);
    int (*read_variant)(void* L, int index, void* variant, int allow_tables);
};

extern struct engine g_engine;

int engine_init(void);
int engine_load_submitted(void* L, const char* name, const char* bytes, size_t size, int env);
int engine_install_bytecode_gate(void);
