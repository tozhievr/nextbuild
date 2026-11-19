#include "nextbuild.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace nb {
std::string utf8(const fs::path& value) {
    const auto bytes = value.u8string();
    return {bytes.begin(), bytes.end()};
}
fs::path path(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + utf8(file));
    std::string result{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw std::runtime_error("cannot read " + utf8(file));
    return result;
}
void write(const fs::path& file, const std::string& data) {
    if (fs::is_regular_file(file) && read(file) == data) return;
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write " + utf8(file));
}
fs::path root_from(fs::path start) {
    start = fs::weakly_canonical(fs::absolute(start));
    for (auto candidate = start;; candidate = candidate.parent_path()) {
        if (fs::is_regular_file(candidate / "tools/build.py") ||
            fs::is_regular_file(candidate / ".nextbuild"))
            return candidate;
        if (candidate == candidate.root_path()) return start;
    }
}
std::string lower(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
bool within(const fs::path& file, const fs::path& directory) {
    const auto relative = file.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
}

SyntaxError::SyntaxError(std::string message, int line, int column)
    : std::runtime_error(std::move(message)), line(line), column(column) {}

namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 128;
}
bool identifier(char c) { return alpha(c) || c == '_'; }

class Parser {
public:
    explicit Parser(std::string_view source) : source(source) {
        if (source.starts_with("\xef\xbb\xbf")) this->source.remove_prefix(3);
    }

    Document document() {
        Document result;
        newlines();
        while (peek()) {
            const int start_line = line, start_column = column;
            const bool variable = peek() == '@';
            auto name = atom();
            if (name.kind != Kind::variable && name.kind != Kind::word)
                fail("expected directive name or @variable");
            expect(':', "expected ':' after assignment target");
            result.push_back({std::move(name.text), variable, start_line, start_column, expression()});
            newlines();
        }
        return result;
    }

private:
    std::string_view source;
    std::size_t offset = 0;
    int line = 1, column = 1;

    char peek(std::size_t ahead = 0) const {
        return ahead < source.size() - offset ? source[offset + ahead] : '\0';
    }
    char take() {
        const char c = peek();
        if (!c) return c;
        ++offset;
        if (c == '\n') {
            ++line;
            column = 1;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            if ((byte & 0xc0) != 0x80) column += byte >= 0xf0 ? 2 : 1;
        }
        return c;
    }
    [[noreturn]] void fail(const char* message) const { throw SyntaxError(message, line, column); }
    void spaces() {
        while (true) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r') take();
            if (peek() != '#' && !(peek() == '/' && peek(1) == '/')) return;
            while (peek() && peek() != '\n') take();
        }
    }
    void newlines() {
        spaces();
        while (peek() == '\n') {
            take();
            spaces();
        }
    }
    void expect(char c, const char* message) {
        spaces();
        if (peek() != c) fail(message);
        take();
    }
    Expr atom() {
        spaces();
        Expr result{Kind::word, {}, {}, {}};
        const char c = peek();
        if (c == '\'' || c == '"') {
            take();
            result.kind = Kind::string;
            while (peek() != c) {
                if (!peek() || peek() == '\n') fail("unterminated string literal");
                char next = take();
                if (next == '\\') {
                    next = take();
                    if (next == 'n') next = '\n';
                    else if (next == 'r') next = '\r';
                    else if (next == 't') next = '\t';
                }
                result.text += next;
            }
            take();
        } else if (c == '@') {
            take();
            result.kind = Kind::variable;
            if (!identifier(peek())) fail("expected identifier after '@'");
            while (identifier(peek()) || digit(peek())) result.text += take();
        } else if (digit(c)) {
            result.kind = Kind::number;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                result.text += take();
                result.text += take();
                if (!std::isxdigit(static_cast<unsigned char>(peek()))) fail("invalid hexadecimal literal");
                while (std::isxdigit(static_cast<unsigned char>(peek()))) result.text += take();
            } else {
                do {
                    result.text += take();
                } while (digit(peek()) || (peek() == '.' && digit(peek(1))));
            }
        } else {
            if (!identifier(c) && c != '+' && c != '-' && c != '.') fail("expected value expression");
            while (identifier(peek()) || digit(peek()) ||
                   (peek() && std::string_view("+-*.").contains(peek()))) result.text += take();
            if (result.text == "true" || result.text == "True" ||
                result.text == "false" || result.text == "False") result.kind = Kind::boolean;
        }
        return result;
    }
    Expr expression(unsigned depth = 0) {
        if (depth == 128) fail("block nesting limit exceeded");
        spaces();
        if (peek() == '{') {
            take();
            Expr result{Kind::block, {}, {}, {}};
            newlines();
            while (peek() != '}') {
                expect('-', "expected '-' list item in block");
                const auto key = atom();
                if (key.kind != Kind::word) fail("expected item name after '-'");
                expect('=', "expected '=' after item name");
                result.keys.push_back(key.text);
                result.parts.push_back(expression(depth + 1));
                newlines();
            }
            take();
            return result;
        }
        auto first = atom();
        spaces();
        if (peek() != '/') return first;
        Expr result{Kind::path, {}, {}, {}};
        result.parts.push_back(std::move(first));
        while (peek() == '/') {
            take();
            result.parts.push_back(atom());
            spaces();
        }
        return result;
    }
};

Address number(const std::string& value) {
    std::size_t end{};
    const auto base = lower(value).starts_with("0x") ? 16 : 10;
    if (value.empty() || value[0] == '-') throw std::runtime_error("invalid address " + value);
    const auto result = std::stoull(value, &end, base);
    if (end != value.size()) throw std::runtime_error("invalid address " + value);
    return result;
}
}
Document parse(std::string_view source) { return Parser(source).document(); }

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
        for (std::size_t index = 0; index < expr.keys.size(); ++index) {
            const auto& key = expr.keys[index];
            const auto address = number(scalar(evaluate(expr.parts[index], vars)));
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

int entry(const nb::Args& args) {
    try {
        if (!args.empty() && args.front() == "lsp") {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            return nb::lsp(nb::Args(args.begin() + 1, args.end()));
        }
        return nb::cli(args);
    } catch (const std::exception& error) {
        std::cerr << "nextbuild: error: " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    nb::Args args;
    for (int index = 1; index < argc; ++index) args.push_back(nb::utf8(nb::fs::path(argv[index])));
    return entry(args);
}
#else
int main(int argc, char** argv) { return entry(nb::Args(argv + 1, argv + argc)); }
#endif
