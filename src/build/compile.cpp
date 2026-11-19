#include "process.hpp"
#include "config.hpp"
#include "json.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace nb {
namespace {
struct Unit {
    fs::path file;
    std::string module, partition;
    Args imports;
    bool bmi = false, assembly = false;
    std::string name() const { return partition.empty() ? module : module + ':' + partition; }
};
bool assembly(const fs::path& p) {
    return p.extension() == ".s" || p.extension() == ".S" || p.extension() == ".asm";
}
bool module_suffix(const fs::path& p) {
    const auto suffix = p.extension();
    return suffix == ".cppm" || suffix == ".ixx" || suffix == ".ccm" || suffix == ".cxxm" || suffix == ".mxx";
}
bool source_suffix(const fs::path& p) {
    const auto suffix = p.extension();
    return assembly(p) || module_suffix(p) || suffix == ".cpp" || suffix == ".cc" ||
           suffix == ".cxx" || suffix == ".c++";
}
Unit scan(const fs::path& file) {
    Unit unit;
    unit.file = file;
    unit.assembly = assembly(file);
    if (unit.assembly) return unit;
    auto text = read(file);
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    static const auto multiline = std::regex_constants::ECMAScript | std::regex_constants::multiline;
    static const std::regex includes(R"(^[ \t]*(#[ \t]*include\b|(?:export\s+)?import\s+[<"]))", multiline);
    static const std::regex declaration(R"(^[ \t]*(export\s+)?module\s+([\w.]+)(?:\s*:\s*([\w.]+))?\s*;)", multiline);
    static const std::regex imports(R"(^[ \t]*(?:export\s+)?import\s+([\w.:]+)\s*;)", multiline);
    if (std::regex_search(text, includes))
        throw std::runtime_error(utf8(file) + ": header imports/#include are not allowed");
    std::smatch match;
    if (std::regex_search(text, match, declaration)) {
        unit.module = match[2].str();
        unit.partition = match[3].str();
        unit.bmi = match[1].matched || !unit.partition.empty();
    } else if (module_suffix(file)) {
        throw std::runtime_error(utf8(file) + ": module interface file without 'export module' declaration");
    }
    for (auto it = std::sregex_iterator(text.begin(), text.end(), imports); it != std::sregex_iterator{}; ++it) {
        auto name = (*it)[1].str();
        unit.imports.push_back(name.starts_with(':') ? unit.module + name : name);
    }
    if (!unit.bmi && !unit.module.empty()) unit.imports.push_back(unit.module);
    return unit;
}
std::vector<Unit> collect(const fs::path& source, const fs::path& build, const fs::path& generated) {
    Paths inputs;
    if (fs::is_directory(source)) {
        for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator{}; ++it) {
            if (it->is_directory() && it->path() == build) it.disable_recursion_pending();
            if (it->is_regular_file() && source_suffix(it->path())) inputs.push_back(it->path());
        }
    } else if (fs::is_regular_file(source)) {
        if (source_suffix(source)) inputs.push_back(source);
    } else throw std::runtime_error("source does not exist: " + utf8(source));
    if (!generated.empty() && std::ranges::find(inputs, generated) == inputs.end()) inputs.push_back(generated);
    std::ranges::sort(inputs);
    std::map<std::string, std::size_t> providers;
    std::vector<Unit> units, ordered;
    for (const auto& input : inputs) {
        auto unit = scan(input);
        if (unit.bmi && !providers.emplace(unit.name(), units.size()).second)
            throw std::runtime_error("duplicate module unit " + unit.name());
        units.push_back(std::move(unit));
    }
    std::vector<int> state(units.size());
    const auto visit = [&](auto&& self, std::size_t index) -> void {
        if (state[index] == 2) return;
        const auto& unit = units[index];
        if (state[index] == 1) throw std::runtime_error("cyclic module import involving '" + unit.name() + "'");
        state[index] = 1;
        for (const auto& name : unit.imports) {
            if (const auto found = providers.find(name); found != providers.end())
                self(self, found->second);
        }
        ordered.push_back(unit);
        state[index] = 2;
    };
    for (const auto& [name, index] : providers) visit(visit, index);
    for (std::size_t i = 0; i < units.size(); ++i) if (!units[i].assembly) visit(visit, i);
    for (std::size_t i = 0; i < units.size(); ++i) visit(visit, i);
    return ordered;
}
std::vector<unsigned long long> version_parts(const std::string& text) {
    static const std::regex pattern(R"([0-9]+(\.[0-9]+)*)");
    if (!std::regex_match(text, pattern)) throw std::runtime_error("invalid version number: " + text);
    std::vector<unsigned long long> result;
    std::istringstream stream(text);
    for (std::string part; std::getline(stream, part, '.');) result.push_back(std::stoull(part));
    return result;
}
std::map<std::string, Json> database(const fs::path& file) {
    std::map<std::string, Json> result;
    if (!fs::is_regular_file(file)) return result;
    try {
        const auto data = JsonParser(read(file)).parse();
        for (const auto& entry : data.array())
            if (auto name = entry["file"].string(); !name.empty()) result[name] = entry;
    } catch (const std::runtime_error&) {}
    return result;
}
void save_database(const fs::path& file, const std::map<std::string, Json>& entries) {
    Json::Array list;
    for (const auto& [name, entry] : entries) list.push_back(entry);
    write(file, Json(std::move(list)).dump() + '\n');
}
void progress(const Config& c, std::size_t done, std::size_t total, const std::string& status, bool quiet) {
    if (quiet || !c.progress) return;
    const auto filled = 28 * done / total;
    std::cout << '\r' << '[' << c.name << "] [" << std::string(filled, '#') << std::string(28-filled, '-')
              << "] " << std::setw(3) << 100*done/total << "% (" << done << '/' << total << ") " << status;
    if (done == total) std::cout << '\n';
    std::cout << std::flush;
}
}

fs::path required_tool(const Args& names, const fs::path& adjacent) {
    for (const auto& name : names)
        if (auto exe = tool(name, adjacent); !exe.empty()) return exe;
    throw std::runtime_error("required tool not found: " + names.front());
}
bool outdated(const fs::path& output, const Paths& inputs) {
    if (!fs::is_regular_file(output)) return true;
    const auto time = fs::last_write_time(output);
    return std::ranges::any_of(inputs, [&](const auto& input) {
        return !fs::is_regular_file(input) || fs::last_write_time(input) > time;
    });
}
void print_command(const char* stage, const Args& args, bool enabled) {
    if (!enabled) return;
    std::cout << "    " << stage;
    for (std::size_t i = 1; i < args.size(); ++i) std::cout << ' ' << args[i];
    std::cout << '\n';
}

fs::path produce(Config& c, const fs::path& root, bool force, bool quiet) {
    if (c.product.empty()) {
        if (!c.layout.empty()) c.product = "nxflat";
        else if (!c.library.empty()) c.product = "library";
        else throw std::runtime_error(utf8(c.file) + ": set SetProductKind or SetExecutableLayout / SetLibraryKind");
    }
    if (c.product == "nxflat" && c.layout.empty())
        throw std::runtime_error("SetExecutableLayout is required for nxflat products");
    if (c.product == "library" && c.library.empty())
        throw std::runtime_error("SetLibraryKind is required for libraries");
    if (!quiet) std::cout << "==> building " << c.name << ' ' << c.version << " [" << c.product << "]\n"
                          << "    source : " << utf8(c.source) << "\n    output : " << utf8(c.output_dir) << '\n';
    const auto compiler = required_tool({c.compiler});
    const auto version_output = run({utf8(compiler), "--version"});
    static const std::regex version_re(R"((?:clang|gcc|g\+\+|Apple LLVM)\s+version\s+(\d+(?:\.\d+)*))", std::regex::icase);
    static const std::regex dotted(R"((\d+(?:\.\d+)+))");
    const auto first = version_output.substr(0, version_output.find('\n'));
    std::smatch match;
    if (!std::regex_search(version_output, match, version_re) && !std::regex_search(first, match, dotted))
        throw std::runtime_error("unable to parse compiler version from: " + version_output);
    const std::string detected = match[1].str();
    if (!c.minimum.empty() && version_parts(detected) < version_parts(c.minimum))
        throw std::runtime_error("compiler " + utf8(compiler) + " version " + detected + " is older than required " + c.minimum);
    if (!quiet) std::cout << "    compiler: " << utf8(compiler) << " (" << detected << ")\n";
    const auto output_dir = c.output_dir;
    const auto obj_dir = output_dir / "obj", pcm_dir = output_dir / "pcm";
    fs::create_directories(obj_dir);
    fs::create_directories(pcm_dir);
    fs::create_directories(output_dir / "generated");
    const auto generated = c.product == "library" ? generate(c) : fs::path{};
    const auto units = collect(c.source, output_dir, generated);
    if (units.empty()) {
        if (!quiet) std::cout << "    warning: no module/asm sources found\n";
        return {};
    }
    const auto triple = c.triple.empty() && (c.product == "nxflat" || (c.product == "library" && c.library == "next"))
        ? std::string("x86_64-unknown-none-elf") : c.triple;
    Args driver{utf8(compiler)};
    if (!triple.empty()) driver.insert(driver.end(), {"-target", triple});
    const auto project_db = output_dir / "compile_commands.json";
    const auto old = database(project_db);
    std::map<std::string, Json> commands;
    std::map<std::string, fs::path> pcms;
    Paths objects, external_pcms;
    for (const auto& dir : c.modules) {
        if (!fs::is_directory(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.is_regular_file() && entry.path().extension() == ".pcm") external_pcms.push_back(entry.path());
    }
    std::size_t compiled = 0, done = 0;
    for (const auto& unit : units) {
        auto relative = unit.file == generated ? fs::path("generated/cmake_generated.cppm") :
            unit.file == c.source ? unit.file.filename() : unit.file.lexically_relative(c.source);
        relative.replace_extension(".o");
        auto object = obj_dir / relative;
        if (std::ranges::find(objects, object) != objects.end())
            throw std::runtime_error("multiple sources produce " + utf8(object));
        fs::create_directories(object.parent_path());
        auto bmi_name = unit.name();
        std::ranges::replace(bmi_name, ':', '-');
        const auto pcm = pcm_dir / path(bmi_name + ".pcm");
        auto args = driver;
        if (!unit.assembly) args.push_back("-std=" + c.standard);
        args.insert(args.end(), c.flags.begin(), c.flags.end());
        for (const auto& dir : c.modules) args.push_back("-fprebuilt-module-path=" + utf8(dir));
        Paths inputs = external_pcms;
        inputs.push_back(unit.file);
        inputs.push_back(compiler);
        for (const auto& [name, bmi] : pcms) {
            args.push_back("-fmodule-file=" + name + "=" + utf8(bmi));
            inputs.push_back(bmi);
        }
        for (const auto& define : c.defines) args.push_back("-D" + define);
        if (module_suffix(unit.file)) args.insert(args.end(), {"-x", "c++-module"});
        else if (unit.file.extension() == ".asm") args.insert(args.end(), {"-x", "assembler"});
        if (unit.bmi) args.push_back("-fmodule-output=" + utf8(pcm));
        const auto source = utf8(unit.file);
        args.insert(args.end(), {"-c", source, "-o", utf8(object)});
        Json::Array arguments;
        for (const auto& arg : args) arguments.emplace_back(arg);
        const auto previous = old.find(source);
        const bool dirty = force || previous == old.end() ||
            previous->second["arguments"].dump() != Json(arguments).dump() ||
            outdated(object, inputs) || (unit.bmi && outdated(pcm, inputs));
        if (dirty) {
            print_command("cm", args, !quiet && !c.progress);
            fs::remove(object);
            if (unit.bmi) fs::remove(pcm);
            try { run(args); }
            catch (...) {
                fs::remove(object);
                if (unit.bmi) fs::remove(pcm);
                throw;
            }
            ++compiled;
        }
        commands[source] = Json::Object{{"directory", utf8(fs::current_path())}, {"file", source}, {"arguments", arguments}};
        if (unit.bmi) pcms[unit.name()] = pcm;
        objects.push_back(object);
        progress(c, ++done, units.size() + 1, utf8(unit.file.filename()) + (dirty ? "" : " (cached)"), quiet);
    }
    save_database(project_db, commands);
    const auto root_db = root / "compile_commands.json";
    if (root_db != project_db) {
        auto merged = database(root_db);
        std::erase_if(merged, [&](const auto& entry) {
            auto file = path(entry.first);
            return within(file, c.source) || within(file, output_dir / "generated");
        });
        for (const auto& [file, entry] : commands) merged.insert_or_assign(file, entry);
        save_database(root_db, merged);
    }
    const auto output = finish_product(c, driver, compiler.parent_path(), objects, force, quiet);
    progress(c, units.size() + 1, units.size() + 1, utf8(output.filename()), quiet);
    const auto tag = c.product == "nxflat" ? "flat" : c.product == "library" ? "lib" : "host";
    if (!quiet) std::cout << "    " << tag << ' ' << utf8(output) << " (" << compiled << " compiled, "
                          << units.size() - compiled << " cached)\n";
    return output;
}
}
