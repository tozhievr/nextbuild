module;
#include <map>
#include <string>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <regex>
#include <sstream>

export module nextbuild.lsp;
import nextbuild.files;
import nextbuild.manifest;
import nextbuild.json;

namespace nb {
struct DirectiveInfo {
    std::string summary, kind, detail, examples;
};
inline const std::map<std::string, DirectiveInfo> catalog{
    {"Project", {"Human-readable project name", "ident", "Sets the project display name. Also used as the default output basename.", "Project : NextBoot"}},
    {"Version", {"Project version", "version", "Dotted version string stored in build metadata.", "Version : 0.1.0"}},
    {"SetBuildRoot", {"Build output directory", "path", "Directory for pcm/obj/segments and the final `.nxflat` image.", "SetBuildRoot : @NEXTOS_MAIN_ROOT / '.build' / 'bootloader'"}},
    {"SetSource", {"Source tree root", "path", "Root scanned for C++20 modules (`.cppm`, `.ixx`, …) and optional asm.", "SetSource : @NEXTOS_MAIN_ROOT / 'bootloader'"}},
    {"SetStandard", {"C++ language standard", "ident", "Passed to the compiler as `-std=<value>`. Prefer `c++20` / `c++2d`.", "SetStandard : c++2d\nSetStandard : c++20"}},
    {"SetCompiler", {"Compiler executable", "ident", "Compiler looked up on PATH (typically `clang++`).", "SetCompiler : clang++"}},
    {"SetMinCompilerVer", {"Minimum compiler version", "version", "Build fails if `clang++ --version` is older than this dotted version.", "SetMinCompilerVer : 23.1.3"}},
    {"SetNextBuildDrawProgress", {"Draw progress bar", "bool", "When `True`, nextbuild draws a progress bar while compiling modules.", "SetNextBuildDrawProgress : True"}},
    {"EnableCompileFlag", {"Add a compile flag", "flag", "Appends one flag to the module compile command. Repeatable.", "EnableCompileFlag : '-ffreestanding'\nEnableCompileFlag : '-fno-exceptions'"}},
    {"EnableLinkFlag", {"Add a link flag", "flag", "Appends one flag to the intermediate link step. Repeatable.", "EnableLinkFlag : '-Wl,-z,max-page-size=0x1000'"}},
    {"AddModulePath", {"Extra BMI/module search path", "path", "Adds `-fprebuilt-module-path=` for consuming prebuilt `.pcm` files.", "AddModulePath : @NEXTOS_MAIN_ROOT / '.build' / 'stdlib' / 'pcm'"}},
    {"AddDefine", {"Preprocessor define", "ident", "Adds `-DNAME` (or `-DNAME=value` if provided as a string).", "AddDefine : NEXTOS_BOOT\nAddDefine : 'NEXTOS_DEBUG=1'"}},
    {"SetOutputName", {"Output basename", "ident", "Overrides the default basename derived from `Project` (without extension).", "SetOutputName : nextboot"}},
    {"SetTargetTriple", {"Clang target triple", "ident", "Optional. Defaults to `x86_64-unknown-none-elf` for freestanding intermediate objects. Final ship format is still NXFL.", "SetTargetTriple : x86_64-unknown-none-elf"}},
    {"SetExecutableLayout", {"Named segments and load addresses", "layout", "Required for `SetProductKind : nxflat`. Declares labeled segments for the NXFL flat image.", "SetExecutableLayout : {\n    - start = 0x10000\n    - main  = 0x12000\n}"}},
    {"SetProductKind", {"Product type: nxflat | library | host", "ident", "`nxflat` — freestanding labeled flat image (OS). `library` — static library + BMI modules. `host` — Native host executable (e.g. VM).", "SetProductKind : library\nSetProductKind : host\nSetProductKind : nxflat"}},
    {"SetLibraryKind", {"Library target profile: host | next", "ident", "`host` — native host static library with cmake_generated. `next` — freestanding NextOS stdlib (ELF intermediates).", "SetLibraryKind : host\nSetLibraryKind : next"}},
    {"EnableFeature", {"Enable a cmake_generated feature tag", "ident", "Repeatable. Host defaults include AVX2 and FMA. Also: SSE42, AVX512, AVX512BW, VNNI.", "EnableFeature : AVX2\nEnableFeature : FMA"}},
    {"UseDependency", {"Consume a .nextbuild-dependency export", "path", "Path to a `.nextbuild-dependency` file. Builds `RequireProject` first, then injects module paths, libraries and flags into this project.", "UseDependency : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host' / '.nextbuild-dependency'"}},
    {"LinkLibrary", {"Link an extra static library", "path", "Repeatable absolute/relative library path passed to the final link step.", "LinkLibrary : @NEXTOS_MAIN_ROOT / '.build' / 'stdlib-host' / 'libcpp26host.lib'"}},
    {"Depend", {"Build another nextbuild project first", "path", "Path to a project directory or `.nextbuild` file that must be built first.", "Depend : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host'"}},
    {"Dependency", {"Dependency export name", "ident", "Human-readable name of this dependency package.", "Dependency : LibCpp26Host"}},
    {"LibraryKind", {"host | next", "ident", "Library profile this dependency provides.", "LibraryKind : host"}},
    {"RequireProject", {"Provider .nextbuild project to build first", "path", "Path to the library project directory or `.nextbuild` file.", "RequireProject : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host'"}},
};
}

namespace nb {
namespace {
using Object = Json::Object;
using Array = Json::Array;
using TextMap = std::map<std::string, std::string>;

std::string trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    return std::string(text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1));
}
Json position(int line, int col) {
    return Object{{"line", line}, {"character", col}};
}
Json range(int line, int col, int length) {
    return Object{{"start", position(line, col)}, {"end", position(line, col + length)}};
}
bool utf8_start(char ch) { return (static_cast<unsigned char>(ch) & 0xc0) != 0x80; }
int utf16_width(char ch) { return static_cast<unsigned char>(ch) >= 0xf0 ? 2 : 1; }
int width(const std::string& text) {
    int result = 0;
    for (const auto ch : text) if (utf8_start(ch)) result += utf16_width(ch);
    return result;
}
std::size_t byte_at(const std::string& text, int character) {
    std::size_t offset = 0;
    while (offset < text.size() && character > 0) {
        character -= utf16_width(text[offset++]);
        while (offset < text.size() && !utf8_start(text[offset])) ++offset;
    }
    return offset;
}
bool word_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || static_cast<unsigned char>(ch) >= 128 || ch == '_';
}
std::string word_at(const std::string& row, std::size_t column) {
    auto first = column;
    while (first > 0 && (word_char(row[first - 1]) || row[first - 1] == '@')) --first;
    auto last = column;
    if (last < row.size() && row[last] == '@') ++last;
    while (last < row.size() && word_char(row[last])) ++last;
    return row.substr(first, last - first);
}
fs::path uri_path(std::string uri) {
    if (!uri.starts_with("file://")) return path(uri);
    uri.erase(0, 7);
    if (uri.starts_with("localhost/")) uri.erase(0, 9);
    if (!uri.starts_with('/')) uri = "//" + uri;
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            unsigned value;
            const auto [end, ec] = std::from_chars(uri.data() + i + 1, uri.data() + i + 3, value, 16);
            if (ec == std::errc{} && end == uri.data() + i + 3) {
                decoded += static_cast<char>(value);
                i += 2;
                continue;
            }
        }
        decoded += uri[i];
    }
#ifdef _WIN32
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(0, 1);
#endif
    return path(decoded);
}
const TextMap builtins{
    {"NEXTOS_MAIN_ROOT", "Absolute path to the NextOS repository root."},
    {"DEP_BUILD_ROOT", "Provider build root (inside a .nextbuild-dependency file)."},
    {"DEP_PCM", "Provider BMI directory."},
    {"DEP_LIBRARY", "Provider static library path."}
};
struct Analysis {
    bool dependency = false;
    std::map<std::string, Statement> variables;
    Array diagnostics;
    const Args& allowed() const { return dependency ? dependency_directives : project_directives; }
    TextMap docs() const {
        auto result = builtins;
        for (const auto& [name, statement] : variables) result[name] = "User variable defined in this document.";
        return result;
    }
};
Analysis analyze(const std::string& text, const std::string& uri) {
    Analysis analysis;
    analysis.dependency = lower(utf8(uri_path(uri).filename())).ends_with(".nextbuild-dependency");
    const auto diagnostic = [&](int line, int col, int length, std::string message) {
        analysis.diagnostics.push_back(Object{{"range", range(line - 1, col - 1, length)},
            {"severity", 1}, {"source", "nextbuild"}, {"message", std::move(message)}});
    };
    try {
        for (const auto& statement : parse(text)) {
            if (statement.variable) analysis.variables.insert_or_assign(statement.name, statement);
            else if (std::ranges::find(analysis.allowed(), statement.name) == analysis.allowed().end())
                diagnostic(statement.line, statement.column, width(statement.name),
                           "unknown directive '" + statement.name + "'");
        }
    } catch (const SyntaxError& error) {
        diagnostic(error.line, error.column, 1, error.what());
    }
    return analysis;
}
Json markdown(std::string text) { return Object{{"kind", "markdown"}, {"value", std::move(text)}}; }
Json directive_markdown(const std::string& name, const DirectiveInfo& info) {
    auto result = "**" + name + "** — " + info.summary + "\n\n" + info.detail + "\n\n_Value kind:_ " + info.kind;
    const std::string fence(3, 96);
    if (!info.examples.empty()) result += "\n\n" + fence + "nextbuild\n" + info.examples + "\n" + fence;
    return markdown(result);
}
Json item(const std::string& label, int kind, const std::string& insert) {
    return Object{{"label", label}, {"kind", kind}, {"insertText", insert}};
}
Json snippet(const std::string& label, const std::string& insert, const std::string& detail = {}, int kind = 15) {
    return Object{{"label", label}, {"kind", kind}, {"insertText", insert},
                  {"insertTextFormat", 2}, {"detail", detail}};
}
constexpr auto root_snippet = "@NEXTOS_MAIN_ROOT / '\x24{1:path}'";
constexpr auto layout_snippet = "{\n    - \x24{1:start} = \x24{2:0x10000}\n}";
Array variables(const Analysis& analysis, std::string partial, bool has_at) {
    Array result;
    partial = lower(partial);
    for (const auto& [name, doc] : analysis.docs())
        if (lower(name).starts_with(partial))
            result.push_back(Object{{"label", "@" + name}, {"kind", 6}, {"detail", doc},
                {"insertText", has_at ? name : "@" + name}, {"filterText", "@" + name}});
    return result;
}
Array paths(const std::string& typed, const fs::path& root, const Analysis& analysis) {
    if (typed.empty()) {
        auto result = variables(analysis, "", false);
        result.push_back(snippet("@NEXTOS_MAIN_ROOT / '…'", root_snippet));
        return result;
    }
    Array result;
    static const std::regex partial(R"((?:^|/\s*)(['"])([^'"]*)$)");
    static const std::regex complete(R"(['"]([^'"]+)['"])");
    std::smatch match;
    std::string needle, prefix = typed;
    if (std::regex_search(typed, match, partial)) {
        needle = lower(match[2].str());
        prefix.resize(static_cast<std::size_t>(match.position(2)));
    } else if (!trim(typed).ends_with('/')) return result;
    auto base = root;
    for (auto it = std::sregex_iterator(prefix.begin(), prefix.end(), complete);
         it != std::sregex_iterator{}; ++it) base /= path((*it)[1].str());
    std::error_code error;
    std::vector<std::pair<fs::path, bool>> entries;
    for (fs::directory_iterator it(base, error); !error && it != fs::directory_iterator{}; it.increment(error)) {
        std::error_code status_error;
        entries.emplace_back(it->path(), it->is_directory(status_error));
    }
    std::ranges::sort(entries, {}, [](const auto& entry) {
        return std::pair(!entry.second, lower(utf8(entry.first.filename())));
    });
    for (const auto& [file, directory] : entries) {
        const auto name = utf8(file.filename());
        if (lower(name).starts_with(needle))
            result.push_back(Object{{"label", name + (directory ? "/" : "")}, {"kind", directory ? 19 : 17},
                {"insertText", name}, {"detail", utf8(within(file, root) ? file.lexically_relative(root) : file)}});
    }
    return result;
}
Array directives(const Analysis& analysis, const std::string& typed) {
    Array result;
    if (typed.empty() && analysis.dependency)
        result.push_back(snippet("dependency skeleton",
            "Dependency : \x24{1:LibName}\nVersion : \x24{2:0.1.0}\nLibraryKind : \x24{3|host,next|}\n\n"
            "RequireProject : @NEXTOS_MAIN_ROOT / '\x24{4:stdlib}' / '\x24{5:host}'\n\n"
            "AddModulePath : @DEP_PCM\nLinkLibrary : @DEP_LIBRARY\n\nEnableCompileFlag : '-nostdinc++'\n",
            "Scaffold a .nextbuild-dependency export"));
    for (const auto& name : analysis.allowed()) {
        if (!lower(name).starts_with(lower(typed))) continue;
        const auto& info = catalog.at(name);
        auto insert = name + " : ";
        if (info.kind == "layout") insert += layout_snippet;
        else if (info.kind == "bool") insert += "\x24{1|True,False|}";
        else if (name == "UseDependency")
            insert += "@NEXTOS_MAIN_ROOT / '\x24{1:stdlib}' / '\x24{2:host}' / '.nextbuild-dependency'";
        else if (name == "SetProductKind") insert += "\x24{1|nxflat,library,host|}";
        else if (name == "SetLibraryKind" || name == "LibraryKind") insert += "\x24{1|host,next|}";
        else if (analysis.dependency && name == "AddModulePath") insert += "@DEP_PCM";
        else if (analysis.dependency && name == "LinkLibrary") insert += "@DEP_LIBRARY";
        else if (info.kind == "path") insert += root_snippet;
        const bool is_snippet = !insert.ends_with(" : ");
        result.push_back(Object{{"label", name}, {"kind", is_snippet ? 15 : 14}, {"detail", info.summary},
            {"documentation", directive_markdown(name, info)}, {"insertText", insert},
            {"insertTextFormat", is_snippet ? 2 : 1}});
    }
    if (typed.empty()) {
        result.push_back(snippet("@VARIABLE", "@\x24{1:NAME} : \x24{2:value}", "Define a nextbuild variable", 6));
        for (auto& variable : variables(analysis, "", false)) result.push_back(std::move(variable));
    }
    return result;
}
Array values(const std::string& directive, const std::string& typed, const Analysis& analysis) {
    static const std::map<std::string, Args> choices{
        {"SetNextBuildDrawProgress", {"True", "False"}},
        {"SetStandard", {"c++26", "c++2c", "c++23", "c++2b", "c++20", "c++2d"}},
        {"SetCompiler", {"clang++", "clang"}},
        {"SetProductKind", {"nxflat", "library", "host"}},
        {"SetLibraryKind", {"host", "next"}}, {"LibraryKind", {"host", "next"}},
        {"EnableFeature", {"SSE42", "AVX2", "FMA", "AVX512", "AVX512BW", "VNNI"}},
        {"EnableCompileFlag", {"'-ffreestanding'", "'-fno-exceptions'", "'-fno-rtti'",
            "'-fno-threadsafe-statics'", "'-nostdinc++'", "'-nobuiltininc'"}}
    };
    Array result;
    if (const auto found = choices.find(directive); found != choices.end()) {
        const bool flag = directive == "EnableCompileFlag";
        const auto begin = typed.find_first_not_of("'\"");
        const auto needle = begin == std::string::npos ? "" : lower(typed.substr(begin));
        const bool boolean = directive == "SetNextBuildDrawProgress";
        for (const auto& value : found->second)
            if (boolean || (flag ? lower(value).contains(needle) : lower(value).starts_with(needle)))
                result.push_back(item(value, boolean ? 21 : 12, value));
        return result;
    }
    if (directive == "SetExecutableLayout" && (typed.empty() || typed == "{"))
        return {snippet("{ layout }", layout_snippet)};
    if (!typed.empty()) return result;
    result = variables(analysis, "", false);
    const std::string base = analysis.dependency ? "@DEP_BUILD_ROOT" : "@NEXTOS_MAIN_ROOT";
    result.push_back(snippet(base + " / '…'", base + " / '\x24{1:path}'"));
    for (const auto* boolean : {"True", "False"}) result.push_back(item(boolean, 21, boolean));
    return result;
}
Array complete(const std::string& prefix, bool layout, const fs::path& root, const Analysis& analysis) {
    static const std::regex layout_prefix(R"(^\s*-?\s*[A-Za-z_]*$)");
    if (layout && !analysis.dependency && std::regex_match(prefix, layout_prefix)) {
        Array result;
        for (std::string name : {"start", "main", "text", "rodata", "data", "bss", "entry"})
            result.push_back(snippet(name, (trim(prefix).starts_with('-') ? "" : "- ") + name +
                " = 0x\x24{1:10000}", "Layout segment label for section ." + name));
        return result;
    }
    const auto at = prefix.rfind('@');
    if (at != std::string::npos && std::ranges::all_of(prefix.substr(at + 1), word_char))
        return variables(analysis, prefix.substr(at + 1), true);
    const auto colon = prefix.find(':');
    if (colon != std::string::npos) {
        const auto name = trim(prefix.substr(0, colon)), typed = trim(prefix.substr(colon + 1));
        const auto info = catalog.find(name);
        if (name.starts_with('@') || typed.contains('/') ||
            (info != catalog.end() && info->second.kind == "path"))
            return paths(typed, root, analysis);
        if (std::ranges::find(analysis.allowed(), name) != analysis.allowed().end())
            return values(name, typed, analysis);
        return {};
    }
    const auto typed = trim(prefix);
    return std::ranges::all_of(typed, word_char) ? directives(analysis, typed) : Array{};
}

void send(Object message) {
    message["jsonrpc"] = "2.0";
    const auto body = Json(std::move(message)).dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}
void reply(const Json& id, Json result) { send({{"id", id}, {"result", std::move(result)}}); }
void fail(const Json& id, int code, std::string message) {
    send({{"id", id}, {"error", Object{{"code", code}, {"message", std::move(message)}}}});
}
bool receive(std::string& body) {
    int length = -1;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty()) break;
        if (!lower(line).starts_with("content-length:")) continue;
        const auto value = trim(line.substr(15));
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
        if (error != std::errc{} || end != value.data() + value.size()) return false;
    }
    if (!std::cin || length < 0 || length > 16 * 1024 * 1024) return false;
    body.resize(static_cast<std::size_t>(length));
    return static_cast<bool>(std::cin.read(body.data(), length));
}
class Server {
public:
    int run() {
        std::string body;
        while (receive(body)) {
            Json message;
            try { message = JsonParser(body).parse(); }
            catch (const std::exception&) { fail(nullptr, -32700, "parse error"); continue; }
            if (message["method"].string() == "exit") return shutdown ? 0 : 1;
            try { dispatch(message); }
            catch (const std::exception& e) {
                if (!std::holds_alternative<std::nullptr_t>(message["id"].data))
                    fail(message["id"], -32603, e.what());
            }
        }
        return 0;
    }
private:
    TextMap documents;
    fs::path root = root_from(fs::current_path());
    bool shutdown = false;

    void diagnostics(const std::string& uri, Array items) {
        send({{"method", "textDocument/publishDiagnostics"},
              {"params", Object{{"uri", uri}, {"diagnostics", std::move(items)}}}});
    }
    void update(const std::string& uri, std::string text) {
        documents[uri] = std::move(text);
        diagnostics(uri, analyze(documents[uri], uri).diagnostics);
    }
    void dispatch(const Json& message) {
        const auto method = message["method"].string();
        const auto& id = message["id"];
        const auto& params = message["params"];
        if (method == "initialize") {
            auto uri = params["rootUri"].string();
            const auto& folders = params["workspaceFolders"].array();
            if (uri.empty() && !folders.empty()) uri = folders.front()["uri"].string();
            if (!uri.empty()) root = root_from(uri_path(uri));
            reply(id, Object{{"capabilities", Object{{"textDocumentSync", 1}, {"hoverProvider", true},
                {"definitionProvider", true}, {"documentSymbolProvider", true},
                {"completionProvider", Object{{"triggerCharacters", Array{"@", "/", "'", "\"", ":", "-", " "}},
                                               {"resolveProvider", false}}}}},
                {"serverInfo", Object{{"name", "nextbuild-ls"}, {"version", "0.1.0"}}}});
            return;
        }
        if (method == "shutdown") {
            shutdown = true;
            reply(id, nullptr);
            return;
        }
        const auto& document = params["textDocument"];
        const auto uri = document["uri"].string();
        if (method == "textDocument/didOpen") update(uri, document["text"].string());
        else if (method == "textDocument/didChange") {
            const auto& changes = params["contentChanges"].array();
            if (!changes.empty()) update(uri, changes.back()["text"].string());
        } else if (method == "textDocument/didClose") {
            documents.erase(uri);
            diagnostics(uri, {});
        } else if (!std::holds_alternative<std::nullptr_t>(id.data)) request(method, params, id, uri);
    }
    void request(const std::string& method, const Json& params, const Json& id, const std::string& uri) {
        const auto found_document = documents.find(uri);
        const auto text = found_document == documents.end() ? "" : found_document->second;
        const auto analysis = analyze(text, uri);
        if (method == "textDocument/documentSymbol") {
            Array symbols;
            for (const auto& [name, variable] : analysis.variables) {
                auto selection = range(variable.line - 1, variable.column - 1, width(name) + 1);
                symbols.push_back(Object{{"name", "@" + name}, {"kind", 13},
                    {"range", selection}, {"selectionRange", selection}});
            }
            reply(id, std::move(symbols));
            return;
        }
        const auto line = params["position"]["line"].integer();
        std::istringstream lines(text);
        std::string row;
        int depth = 0;
        bool seen_layout = false;
        for (int i = 0; i <= line; ++i) {
            if (!std::getline(lines, row)) { row.clear(); break; }
            seen_layout = seen_layout || row.contains("SetExecutableLayout");
            depth += static_cast<int>(std::ranges::count(row, '{') - std::ranges::count(row, '}'));
        }
        const auto column = byte_at(row, params["position"]["character"].integer());
        const auto word = word_at(row, column);
        if (method == "textDocument/completion")
            reply(id, complete(row.substr(0, column), seen_layout && depth > 0, root, analysis));
        else if (method == "textDocument/hover") {
            Json result;
            if (word.starts_with('@')) {
                const auto vars = analysis.docs();
                if (const auto found = vars.find(word.substr(1)); found != vars.end())
                    result = Object{{"contents", markdown("**" + word + "**\n\n" + found->second)}};
            } else if (std::ranges::find(analysis.allowed(), word) != analysis.allowed().end())
                result = Object{{"contents", directive_markdown(word, catalog.at(word))}};
            reply(id, std::move(result));
        } else if (method == "textDocument/definition") {
            Json result;
            if (word.starts_with('@') && !builtins.contains(word.substr(1))) {
                if (const auto found = analysis.variables.find(word.substr(1)); found != analysis.variables.end()) {
                    const auto& variable = found->second;
                    result = Object{{"uri", uri}, {"range", range(variable.line - 1, variable.column - 1, width(word))}};
                }
            }
            reply(id, std::move(result));
        } else fail(id, -32601, "method not found: " + method);
    }
};
}
export int lsp(const Args& args) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: nextbuild lsp [--stdio]\n";
            return 0;
        }
        if (arg != "--stdio") {
            std::cerr << "nextbuild lsp: unknown option " << arg << '\n';
            return 2;
        }
    }
    return Server().run();
}
}
