#pragma once

#include <stddef.h>

// Called only on an attached background worker; returns a JSON response or
// writes an error message. The caller owns the returned buffer.
int device_http_request(const char* request, size_t length, char** response,
                        size_t* response_length, char* error, size_t error_size);
int device_setclipboard(const char* text, size_t length, char* error, size_t error_size);
int device_getclipboard(char** text, size_t* length, char* error, size_t error_size);
int device_fingerprint(char output[97], char* error, size_t error_size);
int device_battery_percent(void);
int device_request_storage_access(char* error, size_t error_size);
int device_open_discord_invite(char* error, size_t error_size);
int device_editor_input(const float bounds[6], int enabled, float state[23], char* error, size_t error_size);
int device_input_event(int kind, int code, float x, float y, float width, float height,
                       char* error, size_t error_size);
int device_input_foreground(void);
