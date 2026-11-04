#include "lua.h"
#include "lualib.h"
#include "lobject.h"
#include "lstate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    FILE* file = fopen(argv[1], "rb");
    if (!file)
        return 2;
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    rewind(file);
    char* bytes = (char*)malloc(size);
    if (fread(bytes, 1, size, file) != size)
        return 2;
    fclose(file);

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int status = luau_load(L, "=basalt", bytes, size, 0);
    printf("load=%d top=%d TValue=%zu Proto=%zu Closure=%zu\n", status, lua_gettop(L), sizeof(TValue), sizeof(Proto), sizeof(Closure));
    if (status == 0)
    {
        Closure* closure = clvalue(L->top - 1);
        Proto* proto = closure->l.p;
        printf("closure proto offset=%zu env offset=%zu proto k=%zu code=%zu p=%zu codeentry=%zu source=%zu\n",
               offsetof(Closure, l.p), offsetof(Closure, env), offsetof(Proto, k),
               offsetof(Proto, code), offsetof(Proto, p), offsetof(Proto, codeentry),
               offsetof(Proto, source));
        printf("closure:");
        for (int i = 0; i < 48; ++i)
            printf(" %02x", ((const uint8_t*)closure)[i]);
        printf("\nproto:");
        for (int i = 0; i < 96; ++i)
            printf(" %02x", ((const uint8_t*)proto)[i]);
        printf("\ncode:");
        for (int i = 0; i < 40; ++i)
            printf(" %02x", ((const uint8_t*)proto->code)[i]);
        puts("");
        printf("isC=%u nup=%u proto_nups=%u maxstack=%u code=%p codeentry=%p sizecode=%d sizep=%d sizek=%d\n",
               closure->isC, closure->nupvalues, proto->nups, proto->maxstacksize,
               (void*)proto->code, (void*)proto->codeentry, proto->sizecode, proto->sizep, proto->sizek);
        for (int i = 0; i < proto->sizecode && i < 8; ++i)
            printf("insn[%d]=0x%08x opcode=%u\n", i, proto->code[i], proto->code[i] & 0xff);
    }
    free(bytes);
    lua_close(L);
    return 0;
}
