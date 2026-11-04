#pragma once

#include <string>
#include <string_view>

// Rewrite only Lua method-call syntax: game:HttpGet(url) -> httpget(url).
// Uses Luau's lexer, leaving string literals/comments and game userdata intact.
std::string rewrite_httpget(std::string_view source);
