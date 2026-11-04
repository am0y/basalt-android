#include "loader.h"
#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct engine g_engine;
int main(int argc, char** argv)
{
    assert(argc > 1);
    for (int a = 1; a < argc; ++a)
    {
        FILE* f = fopen(argv[a], "rb"); assert(f);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f); assert(size > 9);
        rewind(f);
        uint8_t* data = malloc((size_t)size); assert(data);
        assert(fread(data, 1, size, f) == (size_t)size);
        fclose(f);
        size_t prefix = !memcmp(data, "INSPECT\n", 8) ? 8 : 9;
        char error[256];
        if (bytecode_validate(data + prefix, size - prefix, error, sizeof(error)))
        { fprintf(stderr, "%s: %s\n", argv[a], error); return 1; }
        for (size_t n = 0; n < (size_t)size - prefix; ++n)
            assert(bytecode_validate(data + prefix, n, error, sizeof(error)) != 0);
        unsigned random = 512;
        for (unsigned trial = 0; trial < 500; ++trial)
        {
            size_t at = prefix + trial % (size - prefix);
            uint8_t saved = data[at];
            random = random * 1664525u + 1013904223u;
            data[at] = random >> 24;
            bytecode_validate(data + prefix, size - prefix, error, sizeof(error));
            data[at] = saved;
        }
        free(data);
        printf("bounded parse/truncation/mutation OK: %s\n", argv[a]);
    }
    return 0;
}
