#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"
#include "lua.h"
#include "lualib.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

LUAU_FASTFLAG(LuauEmitCallFeedback)
LUAU_FASTFLAG(LuauBytecodeCostModel)
LUAU_FASTFLAG(LuauCompileEmitVectorDouble)

class Encoder final : public Luau::BytecodeEncoder {
    void encode(uint32_t* words, size_t count) override {
        for (size_t pc=0;pc<count;) {
            unsigned op=words[pc]&255;
            words[pc]=(words[pc]&0xffffff00u)|uint8_t(op*227u);
            pc+=Luau::getOpLength(LuauOpcode(op));
        }
    }
};

int main(int argc, char** argv)
{
    if (argc<2) return 2;
    std::string input{std::istreambuf_iterator<char>(std::cin),{}};
    if (std::string(argv[1])=="compile") {
        int version=argc>2 ? std::atoi(argv[2]) : 13;
        FFlag::LuauEmitCallFeedback.value=version>=11;
        FFlag::LuauBytecodeCostModel.value=version>=12;
        FFlag::LuauCompileEmitVectorDouble.value=version>=13;
        Luau::CompileOptions options;
        options.optimizationLevel=2;options.debugLevel=2;options.typeInfoLevel=1;
        options.vectorLib="vector";options.vectorCtor="create";options.vectorType="vector";
        Encoder encoder;
        auto bytes=Luau::compile(input,options,{},&encoder);
        if (bytes.empty() || uint8_t(bytes[0])!=version) return 3;
        std::cout.write(bytes.data(),bytes.size());return 0;
    }
    lua_State* L=luaL_newstate();luaL_openlibs(L);
    int status=luau_load(L,"=compat-contract",input.data(),input.size(),0);
    if (!status) status=lua_pcall(L,0,LUA_MULTRET,0);
    std::printf("status=%d count=%d\n",status,lua_gettop(L));
    for (int i=1;i<=lua_gettop(L);++i) {
        std::printf("%s:",lua_typename(L,lua_type(L,i)));
        if (lua_isnumber(L,i)) std::printf("%.17g",lua_tonumber(L,i));
        else if (lua_isboolean(L,i)) std::printf("%d",lua_toboolean(L,i));
        else if (lua_isstring(L,i)) {
            size_t size=0;const char* data=lua_tolstring(L,i,&size);std::fwrite(data,1,size,stdout);
        }
        std::putchar('\n');
    }
    lua_close(L);return 0;
}
