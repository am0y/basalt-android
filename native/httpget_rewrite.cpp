#include "httpget_rewrite.h"
#include "Luau/Lexer.h"

#include <string>
#include <vector>

std::string rewrite_httpget(std::string_view source)
{
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::Lexer lexer(source.data(), source.size(), names);
    lexer.setSkipComments(true);
    std::vector<Luau::Lexeme> tokens;
    while (true)
    {
        Luau::Lexeme token = lexer.next();
        if (token.type == Luau::Lexeme::Eof) break;
        tokens.push_back(token);
    }
    std::vector<size_t> starts = {0};
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n') starts.push_back(i + 1);
    auto offset = [&](Luau::Position position) {
        return position.line < starts.size() ? starts[position.line] + position.column : source.size();
    };
    auto matches = [&](const Luau::Lexeme& token, std::string_view name) {
        if (token.type != Luau::Lexeme::Name) return false;
        size_t begin = offset(token.location.begin);
        size_t end = offset(token.location.end);
        return end <= source.size() && source.substr(begin, end - begin) == name;
    };
    std::string output;
    size_t cursor = 0;
    for (size_t i = 0; i + 3 < tokens.size(); ++i)
    {
        if (!matches(tokens[i], "game") || tokens[i+1].type != ':' ||
            !(matches(tokens[i+2], "HttpGet") || matches(tokens[i+2], "HttpGetAsync")) ||
            tokens[i+3].type != '(' ||
            (i && (tokens[i-1].type == '.' || tokens[i-1].type == ':' || tokens[i-1].type == Luau::Lexeme::ReservedLocal)))
            continue;
        size_t begin = offset(tokens[i].location.begin);
        size_t end = offset(tokens[i+3].location.begin);
        if (begin < cursor || end > source.size()) continue;
        output.append(source.substr(cursor, begin - cursor));
        output += "httpget";
        cursor = end;
        i += 2;
    }
    output.append(source.substr(cursor));
    return output;
}
