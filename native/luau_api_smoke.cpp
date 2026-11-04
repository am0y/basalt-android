// Exercises the protected C-closure call protocol used by the in-process bridge.
// This is a standalone Luau VM test; it does not start Roblox.
#include "lua.h"
#include "lualib.h"
#include "luacode.h"
#include "ldo.h"
#include "lstate.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct CallContext
{
    StkId func;
    int nresults;
};

static void runCall(lua_State* L, void* data)
{
    CallContext* ctx = static_cast<CallContext*>(data);
    luaD_callint(L, ctx->func, ctx->nresults, false);
}

static int protectedCall(lua_State* L, int nargs, int nresults)
{
    CallContext ctx{L->top - (nargs + 1), nresults};
    ptrdiff_t oldtop = reinterpret_cast<char*>(ctx.func) - reinterpret_cast<char*>(L->stack);
    return luaD_pcall(L, runCall, &ctx, oldtop, 0);
}

static int compileString(lua_State* L)
{
    size_t sourceLength = 0;
    const char* source = luaL_checklstring(L, 1, &sourceLength);
    size_t bytecodeLength = 0;
    char* bytecode = luau_compile(source, sourceLength, nullptr, &bytecodeLength);
    int status = luau_load(L, "=api-smoke", bytecode, bytecodeLength, 0);
    free(bytecode);
    if (status != 0)
        luaL_error(L, "bytecode load failed");
    return 1;
}

static int gatedString(lua_State* L)
{
    luaL_error(L, "loadstring is unavailable");
    return 0;
}

static int calls = 0;

static int dispatch(lua_State* L)
{
    ++calls;
    int initialTop = lua_gettop(L);

    lua_pushcclosurek(L, gatedString, "gatedString", 0, nullptr);
    lua_pushstring(L, "return 1");
    int rejection = protectedCall(L, 1, 1);
    assert(rejection != 0);
    lua_settop(L, initialTop);

    lua_pushcclosurek(L, compileString, "compileString", 0, nullptr);
    lua_pushstring(L, "_G.apiSmokeResult = 'ran from the coroutine caller'");
    int load = protectedCall(L, 1, 1);
    assert(load == 0);
    assert(lua_type(L, -1) == LUA_TFUNCTION);

    int execute = protectedCall(L, 0, 0);
    assert(execute == 0);
    assert(lua_gettop(L) == initialTop);
    return 0;
}

int main()
{
    lua_State* L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    lua_pushcclosurek(L, dispatch, "nativeDispatch", 0, nullptr);
    lua_setglobal(L, "nativeDispatch");

    const char* source =
        "local co = coroutine.create(function() nativeDispatch() end)\n"
        "assert(coroutine.resume(co))\n";
    size_t bytecodeLength = 0;
    char* bytecode = luau_compile(source, strlen(source), nullptr, &bytecodeLength);
    int load = luau_load(L, "=api-smoke", bytecode, bytecodeLength, 0);
    free(bytecode);
    assert(load == 0);
    int status = lua_pcall(L, 0, 0, 0);
    assert(status == 0);
    assert(calls == 1);

    lua_getglobal(L, "apiSmokeResult");
    const char* result = lua_tostring(L, -1);
    assert(result && strcmp(result, "ran from the coroutine caller") == 0);
    puts(result);
    lua_close(L);
    return 0;
}
