#pragma once
void register_http_bridge(void* L, void* env);
#if defined(__x86_64__)
int x86_http_request_start(void* L);
int x86_http_request_poll(void* L);
#endif
