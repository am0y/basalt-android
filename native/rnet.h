#pragma once
#include <stddef.h>
#include <stdint.h>

#define RNET_PACKET_MAX 65535u

int rnet_start(void);
void rnet_stop(int generation);
int rnet_poll(int generation, uint8_t* output, size_t* length);
int rnet_filter(const uint8_t* prefix, size_t length);
int rnet_send(const uint8_t* data, size_t length);
