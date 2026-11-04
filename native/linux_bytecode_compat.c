#include "linux_bytecode_compat.h"
#include "bytecode_layout.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct chunk {
    const uint8_t* input;
    uint8_t* output;
    size_t at, limit, written;
    uint32_t strings, protos;
    const char* error;
    int unsupported;
};

static const uint8_t* take(struct chunk* c, size_t size, int emit)
{
    if (c->error) return NULL;
    if (size > c->limit-c->at) { c->error="truncated bytecode"; return NULL; }
    const uint8_t* data=c->input+c->at;
    c->at+=size;
    if (emit) { memcpy(c->output+c->written,data,size); c->written+=size; }
    return data;
}

static uint8_t byte(struct chunk* c, int emit)
{
    const uint8_t* data=take(c,1,emit);
    return data ? *data : 0;
}

static uint64_t varint(struct chunk* c, unsigned bits, int emit)
{
    uint64_t result=0;
    for (unsigned shift=0;shift<bits;shift+=7) {
        uint8_t b=byte(c,emit);
        if (c->error) return 0;
        if (bits-shift<7 && (b&127u)>=(1u<<(bits-shift))) break;
        result|=(uint64_t)(b&127u)<<shift;
        if (!(b&128u)) return result;
    }
    c->error="bytecode varint overflow";return 0;
}

static uint32_t count(struct chunk* c, size_t stride, int emit)
{
    uint32_t value=(uint32_t)varint(c,32,emit);
    if (!c->error && value>(c->limit-c->at)/stride) c->error="bytecode count exceeds input";
    return c->error ? 0 : value;
}

static uint32_t reference(struct chunk* c, uint32_t limit, int emit)
{
    uint32_t value=(uint32_t)varint(c,32,emit);
    if (value>=limit) c->error="invalid bytecode reference";
    return value;
}

static void string_id(struct chunk* c) { reference(c,c->strings+1,1); }

static void instructions(struct chunk* c, uint32_t size)
{
    size_t output=c->written;
    const uint8_t* code=take(c,(size_t)size*4,1);
    if (!code) return;
    for (uint32_t pc=0;pc<size;) {
        // These are serialized Roblox opcodes, encoded by multiplication by
        // 227, not the engine's in-memory opcode mapping.
        unsigned op=(uint8_t)(code[pc*4]*203u);
        if (op>=OPCODE_COUNT || opcode_lengths[op]>size-pc) {
            c->error="invalid bytecode instruction";return;
        }
        if (op==LOP_CALLFB) {
            // Feedback is profiling metadata. CALL + NOP has identical call
            // behavior and preserves all PCs, jump offsets and debug ranges.
            c->output[output+pc*4]=(uint8_t)(LOP_CALL*227u);
            memset(c->output+output+(pc+1)*4,0,4);
        } else if (op==LOP_FASTPCALL) {
            // FASTPCALL is an optional fast path; on fallthrough the following
            // argument setup and CALL execute normally, so NOP is equivalent.
            c->output[output+pc*4]=0;
            memset(c->output+output+pc*4+1,0,3);
        } else if (op>=LOP_NEWCLASSMEMBER) {
            // Don't guess at lowering newer guards or instructions.
            c->unsupported=1;
        }
        pc+=opcode_lengths[op];
    }
}

static void constants(struct chunk* c, uint32_t proto)
{
    uint32_t size=count(c,1,1);
    for (uint32_t i=0;i<size && !c->error;++i) {
        size_t tag=c->written;
        switch (byte(c,1)) {
        case 0: break;
        case 1: byte(c,1);break;
        case 2: take(c,8,1);break;
        case 3: string_id(c);break;
        case 4: take(c,4,1);break;
        case 5: {
            uint32_t keys=count(c,1,1);
            for (uint32_t k=0;k<keys && !c->error;++k) reference(c,size,1);
            break;
        }
        case 6: reference(c,proto,1);break;
        case 7: take(c,16,1);break;
        case 8: {
            uint32_t keys=count(c,5,1);
            for (uint32_t k=0;k<keys && !c->error;++k) {
                reference(c,size,1);
                const uint8_t* data=take(c,4,1);
                int32_t value=0;if (data) memcpy(&value,data,4);
                if (value>=0 && (uint32_t)value>=size) c->error="invalid table constant";
            }
            break;
        }
        case 9: byte(c,1);varint(c,64,1);break;
        case 10: {
            c->unsupported=1; // class shapes require v10 or later
            reference(c,size,1);
            uint32_t properties=count(c,1,1), methods=count(c,1,1);
            if ((uint64_t)properties+methods>c->limit-c->at) { c->error="invalid class shape";break; }
            for (uint32_t k=0;k<properties+methods && !c->error;++k) reference(c,size,1);
            break;
        }
        case 11: {
            const uint8_t* data=take(c,32,0);
            if (!data) break;
            c->output[tag]=7;
            for (unsigned component=0;component<4;++component) {
                double value;memcpy(&value,data+component*8,8);
                float smaller=(float)value;
                // Preserve precision and signed zero. Chunks requiring actual
                // double vectors retain v13 instead of silently losing data.
                if (!isfinite(value) || (double)smaller!=value || !!signbit(smaller)!=!!signbit(value)) c->unsupported=1;
                memcpy(c->output+c->written,&smaller,4);c->written+=4;
            }
            break;
        }
        default: c->error="unknown bytecode constant";break;
        }
    }
}

static void parse(struct chunk* c, size_t length)
{
    unsigned version=byte(c,1), types=byte(c,1);
    c->output[0]=9;
    if (types<1 || types>3) { c->error="unsupported bytecode type information";return; }
    if (types!=3) c->unsupported=1;
    c->strings=count(c,1,1);
    for (uint32_t i=0;i<c->strings && !c->error;++i) { uint32_t size=count(c,1,1);take(c,size,1); }
    if (types==3) while (byte(c,1) && !c->error) string_id(c);
    c->protos=count(c,1,1);
    if (!c->protos || c->protos>32768) { c->error="invalid bytecode prototype count";return; }
    for (uint32_t i=0;i<c->protos && !c->error;++i) {
        if (version>=12) {
            uint32_t size=count(c,1,0);
            c->limit=c->at+size;
        }
        uint8_t stack=byte(c,1), params=byte(c,1), ups=byte(c,1), variadic=byte(c,1), flags=byte(c,1);
        if (c->error) break;
        if (params>stack || ups>200 || variadic>1) { c->error="invalid bytecode prototype header";break; }
        c->output[c->written-1]&=(uint8_t)~8u; // no inliner cost record in v9
        uint32_t typesize=count(c,1,1);take(c,typesize,1);
        uint32_t code=count(c,4,1);
        if (!code) { c->error="empty bytecode instructions";break; }
        instructions(c,code);
        constants(c,i);
        uint32_t children=count(c,1,1);
        for (uint32_t p=0;p<children && !c->error;++p) reference(c,i,1);
        varint(c,32,1);string_id(c);
        if (byte(c,1)) {
            unsigned gap=byte(c,1);
            if (gap>31) { c->error="invalid bytecode line gap";break; }
            take(c,code,1);take(c,(size_t)(((code-1)>>gap)+1)*4,1);
        }
        if (byte(c,1)) {
            uint32_t locals=count(c,4,1);
            for (uint32_t v=0;v<locals && !c->error;++v) {
                string_id(c);reference(c,code+1,1);reference(c,code+1,1);byte(c,1);
            }
            uint32_t names=count(c,1,1);
            if (names!=ups) c->error="invalid bytecode upvalue names";
            for (uint32_t u=0;u<names && !c->error;++u) string_id(c);
        }
        uint32_t feedback=count(c,2,0);
        for (uint32_t f=0;f<feedback && !c->error;++f) {
            if (byte(c,0)!=0) c->unsupported=1;
            reference(c,code,0);
        }
        if (version>=12) {
            if (flags&8) varint(c,64,0);
            // A future extension inside a known version must not be discarded.
            if (!c->error && c->at!=c->limit) c->unsupported=1;
            c->at=c->limit;c->limit=length;
        }
    }
    if (c->error) return;
    reference(c,c->protos,1);
    // Pinned Roblox Script/CorePackage chunks may carry an opaque 24-byte
    // engine trailer. It isn't part of the portable Luau serialization.
    if (c->at!=length && length-c->at!=24) c->unsupported=1;
}

int linux_bytecode_compat(const char* bytes, size_t length, char** output,
                          size_t* output_length, char* error, size_t capacity)
{
    *output=NULL;*output_length=0;
    if (!length || (uint8_t)bytes[0]<11 || (uint8_t)bytes[0]>14) return 0;
    if (length>4u*1024u*1024u) { snprintf(error,capacity,"bytecode exceeds 4 MiB");return -1; }
    struct chunk c={.input=(const uint8_t*)bytes,.output=malloc(length),.limit=length};
    if (!c.output) { snprintf(error,capacity,"bytecode conversion allocation failed");return -1; }
    parse(&c,length);
        if (c.error || c.unsupported) {
        free(c.output);
        if (c.error) snprintf(error,capacity,"%s",c.error);
        return c.error ? -1 : 0;
    }
    *output=(char*)c.output;*output_length=c.written;return 1;
}
