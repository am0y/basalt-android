#include "httpget_rewrite.h"

#include <cassert>
#include <iostream>
#include <string>

int main()
{
    const std::string source =
        "-- game:HttpGet('untouched')\n"
        "local literal = \"game:HttpGet('untouched')\"\n"
        "local single = game:HttpGet('one', true)\n"
        "local second = game : HttpGetAsync (\"two\")\n"
        "local raw = [=[game:HttpGet('untouched')]=]\n"
        "local property = game.HttpGet\n"
        "local other = getrenv().game:HttpGet('untouched')\n"
        "function game:HttpGet(url) return url end\n";
    const std::string expected = source;
    auto actual = rewrite_httpget(source);
    if (actual != expected)
    {
        std::cerr << "expected:\n" << expected << "actual:\n" << actual;
        return 1;
    }
    for (const std::string sample : {
        "local game={}; return game:HttpGet('a')",
        "local httpget=false; return game:HttpGet('a')",
        "return game:HttpGet 'a'",
        "local alias=game; return alias['Http'..'Get'](alias,'a')"})
        assert(rewrite_httpget(sample) == sample);
    std::cout << "HTTP compilation preserves scope, aliases, syntax, strings and comments\n";
}
