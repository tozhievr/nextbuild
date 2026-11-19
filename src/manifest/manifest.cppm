module;
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cstdint>
#include <exception>
#include <map>
#include <stdexcept>
#include <variant>
#include <algorithm>
#include <ranges>
#include <span>
#include <charconv>
#include "parser.h"
#include "nextbuildmanifest_parse.h"
#include "nextbuildmanifest_sem.h"

export module nextbuild.manifest;
import nextbuild.files;

export namespace nb {
inline const Args project_directives{
    "Project", "Version", "SetBuildRoot", "SetSource", "SetStandard", "SetCompiler",
    "SetMinCompilerVer", "SetNextBuildDrawProgress", "EnableCompileFlag", "EnableLinkFlag",
    "AddModulePath", "AddDefine", "SetOutputName", "SetTargetTriple", "SetExecutableLayout",
    "SetProductKind", "SetLibraryKind", "EnableFeature", "UseDependency", "LinkLibrary", "Depend"
};
inline const Args dependency_directives{
    "Dependency", "Version", "LibraryKind", "RequireProject", "AddModulePath", "LinkLibrary",
    "EnableCompileFlag", "EnableLinkFlag", "AddDefine", "UseDependency"
};

struct SyntaxError : std::runtime_error {
    int line;
    int column;
    SyntaxError(std::string message, int line, int column);
};

enum class Kind { word, string, number, variable, boolean, path, block };

struct Expr {
    Kind kind;
    std::string text;
    std::vector<Expr> parts;
    Args keys;
};

struct Statement {
    std::string name;
    bool variable;
    int line;
    int column;
    Expr expr;
};

using Document = std::vector<Statement>;
using Address = std::uint64_t;
using Layout = std::vector<std::pair<std::string, Address>>;
using Value = std::variant<std::string, fs::path, bool, Address, Layout>;
using Variables = std::map<std::string, Value>;

struct ManifestDiagnostic {
    std::string code;
    std::string message;
    std::size_t offset;
    std::size_t length;
    int severity;
};

Document parse(std::string_view source);
std::vector<ManifestDiagnostic> semantic_diagnostics(std::string_view source);
Value evaluate(const Expr& expr, const Variables& vars);
std::string scalar(const Value& value);
fs::path path_value(const Value& value);
}

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
    case nextbuildmanifest_EXPR_NUMERICWORD: return {number, expr.as.numericword.f0, {}, {}};
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

namespace nb {
namespace {
Address number(const std::string& value) {
    const bool hexadecimal = value.starts_with("0x") || value.starts_with("0X");
    const auto* last = value.data() + value.size();
    const auto* first = value.data() + (hexadecimal ? 2 : 0);
    Address result{};
    if (const auto [end, error] = std::from_chars(first, last, result, hexadecimal ? 16 : 10);
        error == std::errc{} && end == last)
        return result;
    throw std::runtime_error("invalid address " + value);
}
}

std::vector<ManifestDiagnostic> semantic_diagnostics(std::string_view source) {
    constexpr std::size_t max_arena = 64 * 1024 * 1024;
    constexpr std::size_t bom_size = 3;
    const auto bom = source.starts_with("\xef\xbb\xbf") ? bom_size : 0;
    source.remove_prefix(bom);
    struct Collected {
        std::vector<ManifestDiagnostic> diagnostics;
        std::exception_ptr failure;
        std::size_t bom;
    };
    const auto collect = [](void* user, const psl_diag* d) {
        auto& data = *static_cast<Collected*>(user);
        try {
            const auto code = d->def ? d->def->code : psl_str_make("", 0);
            data.diagnostics.push_back({
                std::string(code.ptr, code.len),
                std::string(d->message.ptr, d->message.len),
                data.bom + d->span.offset, d->span.len,
                d->severity == PSL_SEV_ERROR ? 1 : d->severity == PSL_SEV_WARNING ? 2 : 3
            });
        } catch (...) {
            data.failure = std::current_exception();
        }
    };
    for (auto capacity = 256 * 1024uz; capacity <= max_arena; capacity *= 2) {
        std::vector<std::byte> memory(capacity);
        psl_arena arena{};
        psl_arena_init(&arena, memory.data(), memory.size());
        const nextbuildmanifest_node* root = nullptr;
        psl_diag error{};
        const auto parsed = nextbuildmanifest_parse(&arena, psl_str_make(source.data(), source.size()), &root, &error);
        auto* context = parsed == 0 ? nextbuildmanifest_ctx_new(&arena, nullptr) : nullptr;
        Collected data{{}, {}, bom};
        const auto status = context ? nextbuildmanifest_analyze(context, root, collect, &data) : -1;
        if (data.failure) std::rethrow_exception(data.failure);
        if (arena.error == 1) continue;
        if (status < 0) throw std::runtime_error("Parsley semantic analysis failed");
        return data.diagnostics;
    }
    throw std::runtime_error("manifest exceeds the Parsley semantic analysis memory limit");
}

std::string scalar(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* file = std::get_if<fs::path>(&value)) return utf8(*file);
    if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "True" : "False";
    if (const auto* address = std::get_if<Address>(&value)) return std::to_string(*address);
    throw std::runtime_error("expected scalar value");
}
fs::path path_value(const Value& value) {
    if (const auto* file = std::get_if<fs::path>(&value)) return *file;
    if (const auto* text = std::get_if<std::string>(&value)) return path(*text);
    throw std::runtime_error("expected path");
}
Value evaluate(const Expr& expr, const Variables& vars) {
    switch (expr.kind) {
    case Kind::word:
    case Kind::string: return expr.text;
    case Kind::number: return expr.text.contains('.') ? Value(expr.text) : Value(number(expr.text));
    case Kind::boolean: return lower(expr.text) == "true";
    case Kind::variable:
        if (const auto found = vars.find(expr.text); found != vars.end()) return found->second;
        throw std::runtime_error("undefined variable @" + expr.text);
    case Kind::path: {
        fs::path result;
        for (const auto& part : expr.parts) result /= path(scalar(evaluate(part, vars)));
        return result;
    }
    case Kind::block: {
        Layout result;
        for (const auto& [key, part] : std::views::zip(expr.keys, expr.parts)) {
            const auto address = number(scalar(evaluate(part, vars)));
            const auto previous = std::ranges::find(result, key, &Layout::value_type::first);
            if (previous == result.end()) result.emplace_back(key, address);
            else previous->second = address;
        }
        return result;
    }
    }
    throw std::logic_error("unknown expression");
}
}
