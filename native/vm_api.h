#pragma once
#include <stddef.h>
#include <stdint.h>
void register_vm_api(void* L, void* env);
void register_editor_ui_api(void* L, void* env);
int executor_capability_pointer(const uint64_t* pointer);
int executor_trusted_capability_pointer(const uint64_t* pointer);
int executor_prepare_closure(void* L);
int vm_set_thread_identity(void* L, uint32_t identity);
void vm_sync_thread_context(void* L);
int vm_copy_thread_capabilities(void* destination, void* source);
int executor_actor_enqueue(const uintptr_t shared[2], const char* source, size_t length, uint32_t* id);
int executor_actor_poll(uint32_t id, int* status, char* error, size_t capacity, int cancel);
