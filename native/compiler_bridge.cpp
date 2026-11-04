#include "compiler_bridge.h"
#include "httpget_rewrite.h"
#include "numeric_locale.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"
#include <cstdlib>
#include <cstring>
#include <string>

class Encoder final : public Luau::BytecodeEncoder
{
    void encode(uint32_t* data, size_t count) override
    {
        for (size_t i = 0; i < count;)
        {
            uint8_t op = data[i] & 255;
            size_t size = Luau::getOpLength(LuauOpcode(op));
            data[i] = (data[i] & 0xffffff00u) | uint8_t(op * 227u);
            i += size;
        }
    }
};

extern "C" char* compile_encoded(const char* source, size_t length, size_t* output_size)
{
    *output_size = 0;
    try
    {
        NumericLocale numeric;
        if (!numeric) return nullptr;
        Encoder encoder;
#if defined(__x86_64__)
        std::string bytecode = Luau::compile(rewrite_httpget(std::string_view(source, length)), {}, {}, &encoder);
#else
        std::string bytecode = Luau::compile(std::string(source, length), {}, {}, &encoder);
#endif
        char* bytes = static_cast<char*>(std::malloc(bytecode.size()));
        if (bytes)
        {
            std::memcpy(bytes, bytecode.data(), bytecode.size());
            *output_size = bytecode.size();
        }
        return bytes;
    }
    catch (...) { return nullptr; }
}
