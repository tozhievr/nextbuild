#include "manifest.hpp"
#include "parser.h"

#include <algorithm>
#include <ranges>
#include <span>

namespace nb {
SyntaxError::SyntaxError(std::string message, int line, int column)
    : std::runtime_error(std::move(message)), line(line), column(column) {}

namespace {
constexpr std::string_view parser_abi_error = "Parsley AST offsets must be at least 32 bits";
static_assert(sizeof(nextbuildmanifest_ParsleyValue::source_offset) >= 4, parser_abi_error);

class ParserResult {
public:
    explicit ParserResult(std::string_view source)
        : tree(nextbuildmanifest_parser_parse_n(source.data(), source.size())) {}
    ~ParserResult() { nextbuildmanifest_parser_result_free(&tree); }
    ParserResult(const ParserResult&) = delete("a Parsley result owns its AST");
    ParserResult& operator=(const ParserResult&) = delete("a Parsley result owns its AST");

    nextbuildmanifest_ParsleyResult tree;
};

int utf16_column(std::string_view source, int line, int codepoint_column) {
    for (int current = 1; current < line; ++current) {
        const auto newline = source.find('\n');
        if (newline == std::string_view::npos) return 1;
        source.remove_prefix(newline + 1);
    }
    int column = 1;
    for (unsigned char byte : source) {
        if (byte == '\n' || codepoint_column <= 1) break;
        if ((byte & 0xc0) == 0x80) continue;
        column += byte >= 0xf0 ? 2 : 1;
        --codepoint_column;
    }
    return column;
}

int utf16_column_at(std::string_view source, std::size_t offset) {
    const auto previous_line = offset ? source.rfind('\n', offset - 1) : std::string_view::npos;
    const auto start = previous_line == std::string_view::npos ? 0 : previous_line + 1;
    int column = 1;
    for (unsigned char byte : source.substr(start, offset - start)) {
        if ((byte & 0xc0) != 0x80) column += byte >= 0xf0 ? 2 : 1;
    }
    return column;
}

std::string unquote(std::string_view quoted) {
    quoted.remove_prefix(1);
    quoted.remove_suffix(1);
    std::string result;
    while (!quoted.empty()) {
        auto ch = quoted.front();
        quoted.remove_prefix(1);
        if (ch == '\\') {
            ch = quoted.front();
            quoted.remove_prefix(1);
            switch (ch) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            }
        }
        result += ch;
    }
    return result;
}

Expr convert(const nextbuildmanifest_Expr& expr, unsigned depth = 0) {
    if (depth >= 128) throw std::runtime_error("manifest nesting limit exceeded");
    using enum Kind;
    switch (expr.tag) {
    case nextbuildmanifest_EXPR_WORD: return {word, expr.as.word.f0, {}, {}};
    case nextbuildmanifest_EXPR_NEGATIVEWORD:
        return {word, "-" + std::string(expr.as.negativeword.f0), {}, {}};
    case nextbuildmanifest_EXPR_REFERENCE: return {variable, expr.as.reference.f0 + 1, {}, {}};
    case nextbuildmanifest_EXPR_NUMBER: return {number, expr.as.number.f0, {}, {}};
    case nextbuildmanifest_EXPR_QUOTED: return {string, unquote(expr.as.quoted.f0), {}, {}};
    case nextbuildmanifest_EXPR_BOOLEANTRUE: return {boolean, "True", {}, {}};
    case nextbuildmanifest_EXPR_BOOLEANFALSE: return {boolean, "False", {}, {}};
    case nextbuildmanifest_EXPR_PATH: {
        const auto& [first, rest] = expr.as.path;
        auto head = convert(*first, depth + 1);
        if (!rest.len) return head;
        Expr result{path, {}, {}, {}};
        result.parts.reserve(rest.len + 1);
        result.parts.push_back(std::move(head));
        for (const auto* item : std::span(rest.items, rest.len))
            result.parts.push_back(convert(*item, depth + 1));
        return result;
    }
    case nextbuildmanifest_EXPR_BLOCK: {
        const auto& fields = expr.as.block.f0;
        Expr result{block, {}, {}, {}};
        result.parts.reserve(fields.len);
        result.keys.reserve(fields.len);
        for (const auto* field : std::span(fields.items, fields.len)) {
            const auto& [key, value] = field->as.field;
            result.keys.emplace_back(key);
            result.parts.push_back(convert(*value, depth + 1));
        }
        return result;
    }
    }
    throw std::runtime_error("unknown Parsley expression tag");
}
}

Document parse(std::string_view source) {
    if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
    const ParserResult owner(source);
    const auto& parsed = owner.tree;
    if (!parsed.ok) {
        const auto line = std::max(parsed.error.line, 1);
        throw SyntaxError(parsed.error.message ? parsed.error.message : "manifest parse failed",
                          line, utf16_column(source, line, parsed.error.col));
    }
    if (!parsed.root || !parsed.value || parsed.consumed != source.size())
        throw SyntaxError("manifest parse did not consume the input", 1, 1);

    const auto& list = parsed.root->as.manifest.f0;
    const auto& metadata = *parsed.value->items[0];
    if (list.len != metadata.item_count) throw std::logic_error("invalid Parsley statement list");

    Document result;
    result.reserve(list.len);
    const auto statements = std::span(list.items, list.len);
    const auto spans = std::span(metadata.items, metadata.item_count);
    for (const auto& [statement, location] : std::views::zip(statements, spans)) {
        const auto& node = *statement;
        const auto& span = *location;
        const auto line = static_cast<int>(span.source_line);
        const auto column = utf16_column_at(source, span.source_offset);
        if (node.tag == nextbuildmanifest_STATEMENT_VARIABLE) {
            const auto& [name, expression] = node.as.variable;
            result.push_back({name + 1, true, line, column, convert(*expression)});
        } else {
            const auto& [name, expression] = node.as.directive;
            result.push_back({name, false, line, column, convert(*expression)});
        }
    }
    return result;
}
}
