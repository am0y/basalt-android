#include "embedded_bytecode.h"
#include "editor_bytecode.h"
#include "scheduler_bytecode.h"
#include "engine.h"
#include "loader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t load32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint32_t rotate(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32 - count));
}

static void quarter_round(uint32_t words[16], unsigned a, unsigned b, unsigned c, unsigned d)
{
    words[a] += words[b]; words[d] = rotate(words[d] ^ words[a], 16);
    words[c] += words[d]; words[b] = rotate(words[b] ^ words[c], 12);
    words[a] += words[b]; words[d] = rotate(words[d] ^ words[a], 8);
    words[c] += words[d]; words[b] = rotate(words[b] ^ words[c], 7);
}

__attribute__((noinline)) static void wipe(void* memory, size_t length)
{
    volatile uint8_t* bytes = memory;
    while (length--) *bytes++ = 0;
}

static void chacha20_xor(uint8_t* bytes, size_t length, const uint8_t key[32], const uint8_t nonce[12])
{
    uint32_t state[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (unsigned i = 0; i < 8; ++i) state[4 + i] = load32(key + i * 4);
    state[12] = 1;
    for (unsigned i = 0; i < 3; ++i) state[13 + i] = load32(nonce + i * 4);

    for (size_t offset = 0; offset < length; offset += 64)
    {
        uint32_t block[16];
        memcpy(block, state, sizeof(block));
        for (unsigned round = 0; round < 10; ++round)
        {
            quarter_round(block, 0, 4, 8, 12);
            quarter_round(block, 1, 5, 9, 13);
            quarter_round(block, 2, 6, 10, 14);
            quarter_round(block, 3, 7, 11, 15);
            quarter_round(block, 0, 5, 10, 15);
            quarter_round(block, 1, 6, 11, 12);
            quarter_round(block, 2, 7, 8, 13);
            quarter_round(block, 3, 4, 9, 14);
        }
        for (unsigned i = 0; i < 16; ++i) block[i] += state[i];
        size_t remaining = length - offset < 64 ? length - offset : 64;
        for (size_t i = 0; i < remaining; ++i)
            bytes[offset + i] ^= (uint8_t)(block[i / 4] >> ((i % 4) * 8));
        ++state[12];
        wipe(block, sizeof(block));
    }
    wipe(state, sizeof(state));
}

static uint8_t* decrypt(enum embedded_code code, size_t* length)
{
    const uint8_t* encrypted = code == EMBEDDED_EDITOR ? editor_bytecode : scheduler_bytecode;
    const uint8_t* key = code == EMBEDDED_EDITOR ? editor_key : scheduler_key;
    const uint8_t* nonce = code == EMBEDDED_EDITOR ? editor_nonce : scheduler_nonce;
    *length = code == EMBEDDED_EDITOR ? sizeof(editor_bytecode) : sizeof(scheduler_bytecode);
    uint8_t mask_key[32], mask_nonce[12];
    if (*length < sizeof(mask_key)) return NULL;
    for (unsigned i = 0; i < sizeof(mask_key); ++i) mask_key[i] = key[i] ^ encrypted[i];
    for (unsigned i = 0; i < sizeof(mask_nonce); ++i) mask_nonce[i] = nonce[i] ^ encrypted[i];
    uint8_t* plaintext = malloc(*length);
    if (!plaintext)
    {
        wipe(mask_key, sizeof(mask_key));
        wipe(mask_nonce, sizeof(mask_nonce));
        return NULL;
    }
    memcpy(plaintext, encrypted, *length);
    chacha20_xor(plaintext, *length, mask_key, mask_nonce);
    wipe(mask_key, sizeof(mask_key));
    wipe(mask_nonce, sizeof(mask_nonce));
    return plaintext;
}

int load_embedded_bytecode(void* L, const char* name, enum embedded_code code,
                           const uint64_t* capabilities)
{
    size_t length;
    uint8_t* plaintext = decrypt(code, &length);
    if (!plaintext)
    {
        g_engine.lua_pushstring(L, "embedded bytecode allocation failed");
        return 1;
    }
    int status = bytecode_load(L, name, plaintext, length, capabilities);
    wipe(plaintext, length);
    free(plaintext);
    return status;
}
