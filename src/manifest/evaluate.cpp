#include "manifest.hpp"
#include <algorithm>
#include <charconv>
#include <ranges>

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
