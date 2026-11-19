#pragma once

#include "files.hpp"
#include <cstdint>
#include <map>
#include <stdexcept>
#include <variant>

namespace nb {
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

Document parse(std::string_view source);
Value evaluate(const Expr& expr, const Variables& vars);
std::string scalar(const Value& value);
fs::path path_value(const Value& value);
}
