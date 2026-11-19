#include "config.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace nb {
fs::path absolute(fs::path value, const fs::path& root) {
    return fs::weakly_canonical(root / value);
}
Document load_document(const fs::path& file) {
    try {
        return parse(read(file));
    } catch (const SyntaxError& e) {
        throw std::runtime_error(utf8(file) + ':' + std::to_string(e.line) + ':' +
                                 std::to_string(e.column) + ": " + e.what());
    }
}
void require_directive(const Statement& item, const Args& allowed, const fs::path& file) {
    if (std::ranges::find(allowed, item.name) == allowed.end())
        throw std::runtime_error(utf8(file) + ':' + std::to_string(item.line) + ':' +
                                 std::to_string(item.column) + ": unknown directive '" + item.name + "'");
}
Config load_config(const fs::path& file, const fs::path& root) {
    Config c;
    c.file = file;
    Variables vars{{"NEXTOS_MAIN_ROOT", root}};
    const std::map<std::string, std::string Config::*> strings{
        {"Project", &Config::name}, {"Version", &Config::version}, {"SetStandard", &Config::standard},
        {"SetCompiler", &Config::compiler}, {"SetMinCompilerVer", &Config::minimum},
        {"SetOutputName", &Config::output_name}, {"SetTargetTriple", &Config::triple},
        {"SetProductKind", &Config::product}, {"SetLibraryKind", &Config::library}
    };
    const std::map<std::string, Args Config::*> arrays{
        {"EnableCompileFlag", &Config::flags}, {"EnableLinkFlag", &Config::link_flags},
        {"AddDefine", &Config::defines}, {"EnableFeature", &Config::features}
    };
    const std::map<std::string, Paths Config::*> paths{
        {"AddModulePath", &Config::modules}, {"LinkLibrary", &Config::libraries},
        {"UseDependency", &Config::dependencies}, {"Depend", &Config::depends}
    };
    for (const auto& item : load_document(file)) {
        if (item.variable) {
            vars[item.name] = evaluate(item.expr, vars);
            continue;
        }
        require_directive(item, project_directives, file);
        const auto value = evaluate(item.expr, vars);
        if (const auto it = strings.find(item.name); it != strings.end()) c.*it->second = scalar(value);
        else if (const auto it = arrays.find(item.name); it != arrays.end())
            (c.*it->second).push_back(scalar(value));
        else if (const auto it = paths.find(item.name); it != paths.end())
            (c.*it->second).push_back(absolute(path_value(value), root));
        else if (item.name == "SetBuildRoot") c.output_dir = absolute(path_value(value), root);
        else if (item.name == "SetSource") c.source = absolute(path_value(value), root);
        else if (item.name == "SetNextBuildDrawProgress") {
            if (!std::holds_alternative<bool>(value))
                throw std::runtime_error(item.name + " expects a boolean");
            c.progress = std::get<bool>(value);
        } else if (item.name == "SetExecutableLayout") {
            if (!std::holds_alternative<Layout>(value))
                throw std::runtime_error(item.name + " expects a block");
            c.layout = std::get<Layout>(value);
        }
    }
    c.product = lower(c.product);
    c.library = lower(c.library);
    if (!c.library.empty() && c.library != "host" && c.library != "next")
        throw std::runtime_error("SetLibraryKind must be host|next");
    if (!c.product.empty() && c.product != "host" && c.product != "library" && c.product != "nxflat")
        throw std::runtime_error("SetProductKind must be nxflat|library|host");
    if (c.output_name.empty()) c.output_name = lower(c.name);
    if (c.output_dir.empty()) c.output_dir = root / ".build" / path(c.output_name);
    if (c.source.empty()) c.source = file.parent_path();
    return c;
}
Paths discover(const fs::path& root) {
    Paths result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator{}; ++it) {
        if (it->is_directory() && it->path().filename() == "docs") it.disable_recursion_pending();
        if (it->path().filename() == ".nextbuild" && it->is_regular_file() && it->file_size())
            result.push_back(it->path());
    }
    std::ranges::sort(result);
    return result;
}
fs::path project_path(const fs::path& target, const fs::path& root) {
    auto file = absolute(target, root);
    if (fs::is_directory(file)) file /= ".nextbuild";
    if (!utf8(file.filename()).ends_with(".nextbuild"))
        throw std::runtime_error("expected project directory or .nextbuild file: " + utf8(target));
    if (!fs::is_regular_file(file)) throw std::runtime_error(".nextbuild file not found: " + utf8(file));
    return fs::canonical(file);
}

namespace {
template<class T>
void append_unique(std::vector<T>& list, T value) {
    if (std::ranges::find(list, value) == list.end()) list.push_back(std::move(value));
}
struct Result { fs::path output_dir, output; };
class Builder {
public:
    Builder(fs::path root, bool force, bool quiet) : root(std::move(root)), force(force), quiet(quiet) {}
    Result build(const fs::path& target) {
        const auto file = project_path(target, root);
        if (const auto found = built.find(file); found != built.end()) return found->second;
        if (!active.insert(file).second) throw std::runtime_error("cyclic project dependency: " + utf8(file));
        auto c = load_config(file, root);
        for (const auto& dep : c.depends) build(dep);
        std::set<fs::path> used;
        for (const auto& dep : c.dependencies) use(c, dep, used);
        const auto output = produce(c, root, force, quiet);
        active.erase(file);
        return built.emplace(file, Result{c.output_dir, output}).first->second;
    }
    void clean(const fs::path& target) {
        const auto c = load_config(project_path(target, root), root);
        if (!fs::exists(c.output_dir)) return;
        const auto dir = fs::canonical(c.output_dir);
        if (dir == dir.root_path() || within(root, dir) || within(c.file, dir) || within(c.source, dir))
            throw std::runtime_error("refusing to clean a directory containing project sources: " + utf8(dir));
        fs::remove_all(dir);
        if (!quiet) std::cout << "cleaned " << utf8(dir) << '\n';
    }
private:
    fs::path root;
    bool force, quiet;
    std::map<fs::path, Result> built;
    std::set<fs::path> active;

    void use(Config& c, const fs::path& file, std::set<fs::path>& used) {
        if (!used.insert(file).second) return;
        Variables vars{{"NEXTOS_MAIN_ROOT", root}};
        fs::path provider;
        Paths nested;
        Document exports;
        std::string name = "Dependency", version = "0.0.0";
        for (const auto& item : load_document(file)) {
            if (item.variable) {
                vars[item.name] = evaluate(item.expr, vars);
                continue;
            }
            require_directive(item, dependency_directives, file);
            if (item.name == "AddModulePath" || item.name == "LinkLibrary") {
                exports.push_back(item);
                continue;
            }
            const auto value = evaluate(item.expr, vars);
            if (item.name == "RequireProject") provider = path_value(value);
            else if (item.name == "UseDependency") nested.push_back(absolute(path_value(value), root));
            else if (item.name == "EnableCompileFlag") append_unique(c.flags, scalar(value));
            else if (item.name == "EnableLinkFlag") append_unique(c.link_flags, scalar(value));
            else if (item.name == "AddDefine") append_unique(c.defines, scalar(value));
            else if (item.name == "Dependency") name = scalar(value);
            else if (item.name == "Version") version = scalar(value);
        }
        if (!quiet) std::cout << "    use " << name << ' ' << version << " (" << utf8(file) << ")\n";
        if (provider.empty()) throw std::runtime_error(utf8(file) + ": RequireProject is required");
        const auto result = build(provider);
        vars["DEP_BUILD_ROOT"] = result.output_dir;
        vars["DEP_PCM"] = result.output_dir / "pcm";
        vars["DEP_LIBRARY"] = result.output.empty() ? result.output_dir / "library" : result.output;
        for (const auto& item : exports) {
            auto value = absolute(path_value(evaluate(item.expr, vars)), root);
            append_unique(item.name == "AddModulePath" ? c.modules : c.libraries, std::move(value));
        }
        for (const auto& dep : nested) use(c, dep, used);
    }
};
}

int cli(const Args& args) {
    auto root = root_from(fs::current_path());
    bool clean = false, force = false, quiet = false, list = false, options = true;
    Paths targets;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (options && arg == "--") options = false;
        else if (options && (arg == "--help" || arg == "-h")) {
            std::cout << "Usage: nextbuild [--root DIR] [--list] [--clean] [-B|--force] [-q|--quiet] [TARGET ...]\n"
                         "       nextbuild --version\n"
                         "       nextbuild lsp [--stdio]\n";
            return 0;
        } else if (options && arg == "--version") {
            std::cout << "nextbuild 0.1.0\n";
            return 0;
        } else if (options && arg == "--root") {
            if (++index == args.size()) { std::cerr << "nextbuild: --root requires a directory\n"; return 2; }
            root = fs::weakly_canonical(fs::absolute(path(args[index])));
        } else if (options && arg.starts_with("--root=")) root = fs::weakly_canonical(fs::absolute(path(arg.substr(7))));
        else if (options && arg == "--clean") clean = true;
        else if (options && arg == "--list") list = true;
        else if (options && (arg == "-B" || arg == "--force")) force = true;
        else if (options && (arg == "-q" || arg == "--quiet")) quiet = true;
        else if (options && arg.starts_with('-')) {
            std::cerr << "nextbuild: unknown option " << arg << '\n';
            return 2;
        } else targets.push_back(path(arg));
    }
    if (list || targets.empty()) targets = discover(root);
    if (list) {
        if (targets.empty()) std::cout << "no projects under " << utf8(root) << '\n';
        for (const auto& target : targets) std::cout << utf8(target.lexically_relative(root)) << '\n';
        return 0;
    }
    Builder builder(root, force, quiet);
    if (clean) {
        for (const auto& target : targets) builder.clean(target);
        return 0;
    }
    if (targets.empty()) throw std::runtime_error("no .nextbuild files found under " + utf8(root));
    std::size_t built = 0;
    for (const auto& target : targets) if (!builder.build(target).output.empty()) ++built;
    if (!quiet) std::cout << "nextbuild: finished " << built << '/' << targets.size() << " project(s)\n";
    return 0;
}
}
