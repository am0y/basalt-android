#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/Compiler.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "protocol.h"
#include "httpget_rewrite.h"
#include "numeric_locale.h"

class RobloxBytecodeEncoder final : public Luau::BytecodeEncoder
{
    void encode(uint32_t* data, size_t count) override
    {
        for (size_t i = 0; i < count;)
        {
            auto* opcode = reinterpret_cast<uint8_t*>(&data[i]);
            const int length = Luau::getOpLength(static_cast<LuauOpcode>(*opcode));
            *opcode = static_cast<uint8_t>(*opcode * 227u);
            i += length;
        }
    }
};

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 8)
    {
        std::fprintf(stderr, "usage: %s <source.luau> <output.cmd> [schedule|inspect] [--opt=N] [--debug=N] [--type=N] [--x86-httpget]\n", argv[0]);
        return 2;
    }

    bool inspect = false;
    bool schedule = false;
    bool x86_httpget = false;
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    for (int i = 3; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "inspect")
            inspect = true;
        else if (arg == "schedule")
            schedule = true;
        else if (arg == "--opt=0")
            options.optimizationLevel = 0;
        else if (arg == "--opt=1")
            options.optimizationLevel = 1;
        else if (arg == "--opt=2")
            options.optimizationLevel = 2;
        else if (arg == "--debug=0")
            options.debugLevel = 0;
        else if (arg == "--debug=1")
            options.debugLevel = 1;
        else if (arg == "--debug=2")
            options.debugLevel = 2;
        else if (arg == "--type=0")
            options.typeInfoLevel = 0;
        else if (arg == "--type=1")
            options.typeInfoLevel = 1;
        else if (arg == "--x86-httpget")
            x86_httpget = true;
        else
        {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (inspect && schedule)
    {
        std::fprintf(stderr, "choose either schedule or inspect\n");
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
    {
        std::perror(argv[1]);
        return 1;
    }

    const std::string source(std::istreambuf_iterator<char>{input}, {});
    if (source.empty() || input.bad())
    {
        std::fprintf(stderr, "source is empty or could not be read\n");
        return 1;
    }

    RobloxBytecodeEncoder encoder;
    NumericLocale numeric;
    if (!numeric) {
        std::fprintf(stderr, "could not initialize numeric locale\n");
        return 1;
    }
    std::string bytecode = Luau::compile(x86_httpget ? rewrite_httpget(source) : source, options, {}, &encoder);
    if (bytecode.empty() || bytecode[0] == '\0')
    {
        std::fprintf(stderr, "Luau compile failed: %s\n", bytecode.empty() ? "no output" : bytecode.c_str() + 1);
        return 1;
    }


    if (bytecode.size() > COMMAND_MAX_BYTES - 9)
    {
        std::fprintf(stderr, "compiled bytecode exceeds the command limit\n");
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    const char* prefix = inspect ? "INSPECT\n" : schedule ? "SCHEDULE\n" : "EXEC_BIN\n";
    output.write(prefix, inspect ? 8 : 9);
    output.write(bytecode.data(), bytecode.size());
    output.close();
    if (!output)
    {
        std::fprintf(stderr, "could not write %s\n", argv[2]);
        return 1;
    }

    std::printf("%s: Luau v%u, -O%d -g%d -t%d, %zu encoded bytes\n", argv[2], uint8_t(bytecode[0]), options.optimizationLevel, options.debugLevel, options.typeInfoLevel, bytecode.size());
    return 0;
}
