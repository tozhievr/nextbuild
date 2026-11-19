#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace nb {
namespace fs = std::filesystem;
using Args = std::vector<std::string>;

std::string utf8(const fs::path& path);
fs::path path(std::string_view text);
std::string read(const fs::path& file);
void write(const fs::path& file, const std::string& data);
fs::path root_from(fs::path start);
std::string lower(std::string text);
bool within(const fs::path& file, const fs::path& directory);

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
Document parse(std::string_view source);
using Address = std::uint64_t;
using Layout = std::vector<std::pair<std::string, Address>>;
using Value = std::variant<std::string, fs::path, bool, Address, Layout>;
using Variables = std::map<std::string, Value>;
Value evaluate(const Expr& expr, const Variables& vars);
std::string scalar(const Value& value);
fs::path path_value(const Value& value);

std::string run(const Args& args);
fs::path tool(std::string_view name, const fs::path& adjacent = {});
int cli(const Args& args);
int lsp(const Args& args);
}
