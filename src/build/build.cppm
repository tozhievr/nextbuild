module;
#include <charconv>
#include <mutex>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <set>

export module nextbuild.build;
import nextbuild.files;
import nextbuild.manifest;
import nextbuild.json;
import nextbuild.process;
import nextbuild.graph;

namespace nb {
using Paths = std::vector<fs::path>;
namespace {
struct BuildSettings {
    bool stream = false, color = false;
};
BuildSettings settings;

struct LogLine {
    std::ostringstream text;

    template<class T> LogLine& operator<<(const T& value) {
        text << value;
        return *this;
    }

    ~LogLine() { write_output(false, std::move(text).str()); }
};
std::string_view color(std::string_view sequence) {
    return settings.color ? sequence : std::string_view{};
}
}
struct Config {
    fs::path file, source, output_dir, version_file;
    std::string name = "Untitled", version = "0.0.0", output_name;
    std::string product, library, compiler = "clang++", minimum, standard = "c++20", triple;
    bool progress = false, platform_module = false;
    Args flags, link_flags, defines, features;
    Paths modules, libraries, depends, dependencies, extra_sources;
    Layout layout;
};
fs::path absolute(fs::path value, const fs::path& root);
Document load_document(const fs::path& file);
void require_directive(const Statement& item, const Args& allowed, const fs::path& file);
Config load_config(const fs::path& file, const fs::path& root);
Paths discover(const fs::path& root);
fs::path project_path(const fs::path& target, const fs::path& root);
fs::path produce(Config& c, const fs::path& root, bool force, bool quiet, unsigned jobs);
fs::path finish_product(Config& c, Args args, const fs::path& bindir,
                        const Paths& objects, bool force, bool quiet);
fs::path generate(const Config& c);
fs::path required_tool(const Args& names, const fs::path& adjacent = {});
bool outdated(const fs::path& output, const Paths& inputs);
void print_command(const char* stage, const Args& args, bool enabled);
}

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
           suffix == ".cxx" || suffix == ".c++" || suffix == ".c";
}
Unit scan(const fs::path& file) {
    Unit unit;
    unit.file = file;
    unit.assembly = assembly(file);
    if (unit.assembly || file.extension() == ".c") return unit;
    auto text = read(file);
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    static const auto multiline = std::regex_constants::ECMAScript | std::regex_constants::multiline;
    static const std::regex declaration(R"(^[ \t]*(export\s+)?module\s+([\w.]+)(?:\s*:\s*([\w.]+))?\s*;)", multiline);
    static const std::regex imports(R"(^[ \t]*(?:export\s+)?import\s+([\w.:]+)\s*;)", multiline);
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
std::vector<Unit> collect(const Paths& sources, const fs::path& build, const fs::path& generated) {
    Paths inputs;
    for (const auto& source : sources) {
        if (fs::is_directory(source)) {
            for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator{}; ++it) {
                if (it->is_directory() && it->path() == build) it.disable_recursion_pending();
                if (it->is_regular_file() && source_suffix(it->path())) inputs.push_back(it->path());
            }
        } else if (fs::is_regular_file(source)) {
            if (source_suffix(source)) inputs.push_back(source);
        } else throw std::runtime_error("source does not exist: " + utf8(source));
    }
    if (!generated.empty() && std::ranges::find(inputs, generated) == inputs.end()) inputs.push_back(generated);
    std::ranges::sort(inputs);
    inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
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
    if (!quiet && c.progress)
        LogLine() << "    [" << c.name << ' ' << done << '/' << total << "] " << status << '\n';
}

bool needs_platform_module(const Config& config, const std::vector<Unit>& units) {
    const bool library = config.product == "library" ||
        (config.product.empty() && config.layout.empty() && !config.library.empty());
    if (!library) return false;
    return config.platform_module || !config.features.empty() ||
        std::ranges::any_of(units, [](const Unit& unit) {
            return std::ranges::find(unit.imports, "cmake_generated") != unit.imports.end();
        });
}

graph::Edges unit_dependencies(const std::vector<Unit>& units, bool transitive = true) {
    graph::Edges dependencies(units.size());
    std::map<std::string, std::size_t> providers;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        std::set<std::size_t> prerequisites;
        for (const auto& imported : unit.imports) {
            if (const auto provider = providers.find(imported); provider != providers.end()) {
                const auto previous = provider->second;
                prerequisites.insert(previous);
                if (transitive) prerequisites.insert(dependencies[previous].begin(), dependencies[previous].end());
            }
        }
        dependencies[index].assign(prerequisites.begin(), prerequisites.end());
        if (unit.bmi) providers.emplace(unit.name(), index);
    }
    return dependencies;
}
}

fs::path required_tool(const Args& names, const fs::path& adjacent) {
    for (const auto& name : names)
        if (auto exe = tool(name, adjacent); !exe.empty()) return exe;
    throw std::runtime_error("required tool not found: " + names.front());
}
namespace {
std::optional<Paths> read_dependencies(const fs::path& depfile) {
    constexpr std::string_view target = "nextbuild-object:";
    if (!fs::is_regular_file(depfile)) return std::nullopt;
    const auto source = read(depfile);
    if (!source.starts_with(target)) return std::nullopt;
    Paths result;
    std::string word;
    const auto store = [&] {
        if (!word.empty()) result.push_back(path(std::exchange(word, {})));
    };
    for (auto i = target.size(); i < source.size(); ++i) {
        const auto c = source[i];
        if (c == '\\' && i + 1 < source.size()) {
            const auto next = source[i + 1];
            if (next == '\n' || next == '\r') {
                ++i;
                if (next == '\r' && i + 1 < source.size() && source[i + 1] == '\n') ++i;
                store();
                continue;
            }
            if (next == ' ' || next == '\t' || next == '#' || next == '\\') {
                word += next;
                ++i;
                continue;
            }
        }
        if (c == '$' && i + 1 < source.size() && source[i + 1] == '$') ++i;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') store();
        else word += c;
    }
    store();
    return result;
}
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
    LogLine output;
    output << "    " << color("\x1b[36m") << stage << color("\x1b[0m");
    for (std::size_t i = 1; i < args.size(); ++i) output << ' ' << args[i];
    output << '\n';
}

fs::path produce(Config& c, const fs::path& root, bool force, bool quiet, unsigned jobs) {
    if (c.product.empty()) {
        if (!c.layout.empty()) c.product = "nxflat";
        else if (!c.library.empty()) c.product = "library";
        else c.product = "host";
    }
    if (c.product == "nxflat" && c.layout.empty())
        throw std::runtime_error("SetExecutableLayout is required for nxflat products");
    if (c.product == "library" && c.library.empty()) c.library = "host";
    if (!quiet) LogLine() << color("\x1b[1;34m") << "==> building " << color("\x1b[0m") << c.name << ' ' << c.version << " [" << c.product << "]\n"
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
    if (!quiet) LogLine() << "    compiler: " << utf8(compiler) << " (" << detected << ")\n";
    const auto output_dir = c.output_dir;
    const auto obj_dir = output_dir / "obj", pcm_dir = output_dir / "pcm";
    fs::create_directories(obj_dir);
    fs::create_directories(pcm_dir);
    fs::create_directories(output_dir / "generated");
    fs::path generated;
    Paths sources{c.source};
    sources.insert(sources.end(), c.extra_sources.begin(), c.extra_sources.end());
    auto units = collect(sources, output_dir, {});
    if (needs_platform_module(c, units)) {
        generated = generate(c);
        units = collect(sources, output_dir, generated);
    }
    if (units.empty()) {
        if (!quiet) LogLine() << "    warning: no module/asm sources found\n";
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
    struct Compilation {
        Args args;
        Paths inputs;
        fs::path object, pcm, depfile;
        bool track_headers, bmi;
        std::string source;
    };
    const auto deps = unit_dependencies(units);
    std::vector<Compilation> commands_to_run;
    commands_to_run.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        fs::path relative;
        if (unit.file == generated) relative = "generated/cmake_generated.cppm";
        else for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            if (!within(unit.file, source)) continue;
            relative = unit.file == source ? unit.file.filename() : unit.file.lexically_relative(source);
            if (i) relative = fs::path("extra") / std::to_string(i) / relative;
            break;
        }
        if (relative.empty()) throw std::logic_error("source is outside the configured source roots");
        relative.replace_extension(".o");
        auto object = obj_dir / relative;
        if (std::ranges::find(objects, object) != objects.end())
            throw std::runtime_error("multiple sources produce " + utf8(object));
        fs::create_directories(object.parent_path());
        auto bmi_name = unit.name();
        std::ranges::replace(bmi_name, ':', '-');
        const auto pcm = pcm_dir / path(bmi_name + ".pcm");
        auto args = driver;
        if (unit.file.extension() == ".c") args.insert(args.end(), {"-x", "c", "-std=c23"});
        else if (!unit.assembly) args.push_back("-std=" + c.standard);
        const bool track_headers = !unit.assembly || unit.file.extension() == ".S";
        auto depfile = object;
        depfile += ".d";
        if (track_headers) args.insert(args.end(), {"-MD", "-MF", utf8(depfile), "-MT", "nextbuild-object"});
        args.insert(args.end(), c.flags.begin(), c.flags.end());
        Paths inputs{unit.file, compiler};
        if (!unit.assembly && unit.file.extension() != ".c") {
            for (const auto& dir : c.modules) args.push_back("-fprebuilt-module-path=" + utf8(dir));
            inputs.insert(inputs.end(), external_pcms.begin(), external_pcms.end());
            for (const auto dependency : deps[index]) {
                const auto& provider = units[dependency];
                const auto& bmi = pcms.at(provider.name());
                args.push_back("-fmodule-file=" + provider.name() + "=" + utf8(bmi));
                inputs.push_back(bmi);
            }
        }
        for (const auto& define : c.defines) args.push_back("-D" + define);
        if (module_suffix(unit.file)) args.insert(args.end(), {"-x", "c++-module"});
        else if (unit.file.extension() == ".asm") args.insert(args.end(), {"-x", "assembler"});
        if (unit.bmi) args.push_back("-fmodule-output=" + utf8(pcm));
        const auto source = utf8(unit.file);
        args.insert(args.end(), {"-c", source, "-o", utf8(object)});
        commands_to_run.push_back({std::move(args), std::move(inputs), object, pcm, depfile,
                                   track_headers, unit.bmi, source});
        if (unit.bmi) pcms[unit.name()] = pcm;
        objects.push_back(object);
    }

    std::vector<unsigned char> rebuilt(units.size());
    std::vector<Json> entries(units.size());
    std::mutex progress_mutex;
    std::size_t done = 0;
    const auto directory = utf8(root);
    graph::execute(deps, jobs, [&](std::size_t index) {
        const auto& task = commands_to_run[index];
        auto inputs = task.inputs;
        const auto headers = task.track_headers ? read_dependencies(task.depfile) : std::nullopt;
        if (headers) inputs.insert(inputs.end(), headers->begin(), headers->end());
        Json::Array arguments;
        for (const auto& arg : task.args) arguments.emplace_back(arg);
        const auto previous = old.find(task.source);
        const bool upstream_rebuilt = std::ranges::any_of(deps[index], [&](std::size_t i) { return rebuilt[i] != 0; });
        const bool dirty = force || upstream_rebuilt || (task.track_headers && !headers) ||
            previous == old.end() || previous->second["arguments"].dump() != Json(arguments).dump() ||
            outdated(task.object, inputs) || (task.bmi && outdated(task.pcm, inputs));
        if (dirty) {
            print_command("cm", task.args, !quiet && !c.progress);
            const auto cleanup = [&] {
                fs::remove(task.object);
                if (task.bmi) fs::remove(task.pcm);
                if (task.track_headers) fs::remove(task.depfile);
            };
            cleanup();
            try { run(task.args, settings.stream); }
            catch (...) {
                cleanup();
                throw;
            }
            rebuilt[index] = 1;
        }
        entries[index] = Json::Object{{"directory", directory}, {"file", task.source}, {"arguments", std::move(arguments)}};
        if (c.progress && !quiet) {
            std::scoped_lock lock(progress_mutex);
            progress(c, ++done, units.size() + 1, utf8(units[index].file.filename()) + (dirty ? "" : " (cached)"), quiet);
        }
    });
    const auto compiled = static_cast<std::size_t>(std::ranges::count(rebuilt, 1));
    for (std::size_t i = 0; i < units.size(); ++i)
        commands.emplace(commands_to_run[i].source, std::move(entries[i]));
    save_database(project_db, commands);

    const auto output = finish_product(c, driver, compiler.parent_path(), objects, force, quiet);
    progress(c, units.size() + 1, units.size() + 1, utf8(output.filename()), quiet);
    const auto tag = c.product == "nxflat" ? "flat" : c.product == "library" ? "lib" : "host";
    if (!quiet) LogLine() << "    " << color("\x1b[32m") << tag << color("\x1b[0m") << ' ' << utf8(output) << " (" << compiled << " compiled, "
                          << units.size() - compiled << " cached)\n";
    return output;
}
}

namespace nb {
namespace {
#ifdef _WIN32
constexpr bool windows = true;
#else
constexpr bool windows = false;
#endif
std::string hex(Address n) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << n;
    return out.str();
}
void little(std::string& data, Address value, int bytes) {
    for (int index = 0; index < bytes; ++index) {
        data += static_cast<char>(value & 255);
        value >>= 8;
    }
}
std::string sections(const Layout& layout) {
    std::string script = "OUTPUT_FORMAT(\"elf64-x86-64\")\nENTRY(" + layout.front().first + ")\nSECTIONS\n{\n";
    for (const auto& [name, address] : layout) {
        constexpr std::string_view initial = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
        const auto valid = name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789.");
        if (name.empty() || !initial.contains(name.front()) || valid != std::string::npos)
            throw std::runtime_error("invalid section label " + name);
        script += "  ." + name + " " + hex(address) + " : { KEEP(*(." + name +
                  ")) KEEP(*(.text." + name + ")) *(.text." + name + ".*) }\n";
    }
    return script + "  .text : { *(.text .text.*) }\n  .rodata : { *(.rodata .rodata.*) }\n"
                    "  .data : { *(.data .data.*) }\n  .bss : { *(.bss .bss.*) *(COMMON) }\n}\n";
}
void pack(const Layout& layout, const fs::path& intermediate, const fs::path& objcopy,
          const fs::path& output, const fs::path& sidecar) {
    auto directory = output.parent_path() / "segments";
    fs::create_directories(directory);
    Address offset = 16;
    for (const auto& [name, address] : layout) {
        if (name.size() > UINT16_MAX) throw std::runtime_error("section label too long");
        offset += 26 + name.size();
    }
    if (offset > UINT32_MAX) throw std::runtime_error("NXFL label table too large");
    std::string header = "NXFL", payload;
    little(header, 1, 4);
    little(header, layout.size(), 4);
    little(header, offset, 4);
    Json::Array labels;
    Json::Object addresses;
    for (const auto& [name, address] : layout) {
        const auto blob = directory / path(name + ".bin");
        fs::remove(blob);
        run({utf8(objcopy), "-O", "binary", "-j", "." + name, utf8(intermediate), utf8(blob)}, settings.stream);
        const auto contents = fs::is_regular_file(blob) ? read(blob) : std::string{};
        little(header, name.size(), 2);
        header += name;
        little(header, address, 8);
        little(header, offset + payload.size(), 8);
        little(header, contents.size(), 8);
        addresses[name] = hex(address);
        labels.push_back(Json::Object{{"name", name}, {"address", hex(address)}, {"address_int", address},
            {"payload_offset", Address(payload.size())}, {"size", Address(contents.size())}});
        payload += contents;
    }
    write(sidecar, Json(Json::Object{{"format", "NXFL"}, {"version", 1},
                                    {"layout", addresses}, {"labels", labels}}).dump() + '\n');
    write(output, header + payload);
}
void execute(const Args& args, const fs::path& output, const char* stage, bool verbose) {
    print_command(stage, args, verbose);
    try { run(args, settings.stream); }
    catch (...) {
        fs::remove(output);
        throw;
    }
}
std::string signature(const Args& args) {
    Json::Array array;
    for (const auto& arg : args) array.emplace_back(arg);
    return Json(std::move(array)).dump();
}
bool changed(const fs::path& stamp, const fs::path& output, const Args& args, const Paths& inputs, bool force) {
    return force || !fs::is_regular_file(stamp) || read(stamp) != signature(args) || outdated(output, inputs);
}
}

fs::path generate(const Config& c) {
    const auto file = c.output_dir / "generated/cmake_generated.cppm";
    const Args known{"SSE42", "AVX2", "AVX512", "AVX512BW", "FMA", "VNNI"};
    const bool host = c.library == "host";
    std::string text = "export module cmake_generated;\nexport namespace cmake_generated {\n";
    for (const auto tag : {"PlatformWindows", "PlatformLinux", "PlatformApple", "EndianessLittle", "EndianessBig"})
        text += "struct " + std::string(tag) + " {};\n";
    for (const auto& [macro, type] : {std::pair{"_WIN32", "Windows"},
                                     {"__linux__", "Linux"}, {"__APPLE__", "Apple"}}) {
        auto declaration = "inline constexpr bool kIs" + std::string(type) + "Platform = ";
        if (host) text += "#if defined(" + std::string(macro) + ")\n" + declaration + "true;\n#else\n";
        text += declaration + "false;\n";
        if (host) text += "#endif\n";
    }
    text += "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n"
            "inline constexpr bool kIsLittleEndian = false;\n#else\n"
            "inline constexpr bool kIsLittleEndian = true;\n#endif\n"
            "inline constexpr bool kIsBigEndian = !kIsLittleEndian;\n"
            "template<class Feature> struct FeatureEnabled { static constexpr bool value = false; };\n";
    for (const auto& feature : known) {
        const bool default_on = host && (feature == "AVX2" || feature == "FMA");
        const bool selected = std::ranges::any_of(c.features, [&](const auto& f) {
            return lower(f) == lower(feature);
        });
        text += "struct " + feature + " {};\n";
        const auto specialization = "template<> struct FeatureEnabled<" + feature +
            "> { static constexpr bool value = true; };\n";
        if (selected) text += specialization;
        else if (default_on)
            text += "#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)\n" +
                    specialization + "#endif\n";
    }
    text += "template<class Feature> inline constexpr bool kIsEnabledFeature = FeatureEnabled<Feature>::value;\n}\n";
    write(file, text);
    return file;
}

fs::path finish_product(Config& c, Args args, const fs::path& bindir,
                        const Paths& objects, bool force, bool quiet) {
    const auto dir = c.output_dir;
    auto output = dir / path(c.output_name);
    const bool verbose = !quiet && !c.progress;
    const auto append_paths = [&](const Paths& paths) {
        for (const auto& p : paths) args.push_back(utf8(p));
    };
    if (c.product == "library") {
        const bool coff_name = windows && (c.library == "host" || c.triple.empty());
        output = dir / path(coff_name ? c.output_name + ".lib" : "lib" + c.output_name + ".a");
        args = {utf8(required_tool({"llvm-ar", "ar"}, bindir)), "rcs", utf8(output)};
        append_paths(objects);
        const auto stamp = dir / "archive-command.json";
        if (changed(stamp, output, args, objects, force)) {
            fs::remove(output);
            execute(args, output, "ar", verbose);
            write(stamp, signature(args));
        }
        return output;
    }
    if (c.product == "host") {
        if (windows && !output.has_extension()) output += ".exe";
        const auto bare = [](const auto& flag) {
            return flag == "-ffreestanding" || flag.starts_with("-nostd");
        };
        if (std::ranges::any_of(c.flags, bare) || std::ranges::any_of(c.link_flags, bare)) {
            if (!std::ranges::any_of(c.link_flags, [](const auto& f){ return f.starts_with("-fuse-ld="); }))
                args.push_back("-fuse-ld=lld");
            if (std::ranges::find(c.link_flags, "-nostdlib") == c.link_flags.end()) args.push_back("-nostdlib");
            if (windows && !std::ranges::any_of(c.link_flags, [](const auto& f){ return lower(f).contains("subsystem"); }))
                args.push_back("-Wl,/subsystem:console");
        }
    } else {
        std::ranges::stable_sort(c.layout, {}, &Layout::value_type::second);
        const auto script = dir / "sections.ld";
        write(script, sections(c.layout));
        args = {utf8(required_tool({"ld.lld"}, bindir)), "-static", "-T", utf8(script),
                "--build-id=none", "--no-dynamic-linker"};
        output = dir / path(c.output_name + ".nxflat");
    }
    if (c.product == "host") args.insert(args.end(), c.link_flags.begin(), c.link_flags.end());
    else for (const auto& flag : c.link_flags) {
        if (flag.starts_with("-Wl,")) {
            std::string_view forwarded(flag);
            forwarded.remove_prefix(4);
            while (!forwarded.empty()) {
                const auto comma = forwarded.find(',');
                args.emplace_back(forwarded.substr(0, comma));
                if (comma == std::string_view::npos) break;
                forwarded.remove_prefix(comma + 1);
            }
        } else if (flag != "-Xlinker" && flag != "-nostdlib" && flag != "-nostartfiles" &&
                   flag != "-nodefaultlibs" && !flag.starts_with("-fuse-ld=")) {
            args.push_back(flag);
        }
    }
    append_paths(objects);
    append_paths(c.libraries);
    const auto intermediate = c.product == "nxflat" ? dir / "intermediate.elf" : output;
    args.insert(args.end(), {"-o", utf8(intermediate)});
    if (c.product == "host") execute(args, output, "ld", verbose);
    else {
        const auto sidecar = dir / path(c.output_name + ".labels.json");
        const auto stamp = dir / "flat-command.json";
        auto inputs = objects;
        inputs.insert(inputs.end(), c.libraries.begin(), c.libraries.end());
        inputs.push_back(dir / "sections.ld");
        if (changed(stamp, output, args, inputs, force) || !fs::is_regular_file(sidecar)) {
            fs::remove(output);
            execute(args, intermediate, "ld", verbose);
            pack(c.layout, intermediate, required_tool({"llvm-objcopy", "objcopy"}, bindir), output, sidecar);
            write(stamp, signature(args));
        }
    }
    return output;
}
}

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
    Variables vars{{"PROJECT_ROOT", root}, {"NEXTOS_MAIN_ROOT", root}};
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
        else if (item.name == "AddSource") c.extra_sources.push_back(absolute(path_value(value), root));
        else if (item.name == "SetVersionFile") c.version_file = absolute(path_value(value), root);
        else if (item.name == "SetNextBuildDrawProgress" || item.name == "SetGeneratePlatformModule") {
            if (!std::holds_alternative<bool>(value))
                throw std::runtime_error(item.name + " expects a boolean");
            if (item.name == "SetGeneratePlatformModule") c.platform_module = std::get<bool>(value);
            else c.progress = std::get<bool>(value);
        } else if (item.name == "SetExecutableLayout") {
            if (!std::holds_alternative<Layout>(value))
                throw std::runtime_error(item.name + " expects a block");
            c.layout = std::get<Layout>(value);
        }
    }
    c.product = lower(c.product);
    c.library = lower(c.library);
    if (c.library == "freestanding") c.library = "next";
    if (!c.library.empty() && c.library != "host" && c.library != "next")
        throw std::runtime_error("SetLibraryKind must be host|freestanding");
    if (!c.product.empty() && c.product != "host" && c.product != "library" && c.product != "nxflat")
        throw std::runtime_error("SetProductKind must be nxflat|library|host");
    if (c.output_name.empty()) c.output_name = lower(c.name);
    if (c.output_dir.empty()) c.output_dir = root / ".build" / path(c.output_name);
    if (c.source.empty()) c.source = file.parent_path();
    if (!c.version_file.empty()) {
        auto source = read(c.version_file);
        const auto end = source.find_last_not_of(" \t\r\n");
        source.resize(end == std::string::npos ? 0 : end + 1);
        static const std::regex version_pattern(R"((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))");
        if (!std::regex_match(source, version_pattern))
            throw std::runtime_error("invalid version in " + utf8(c.version_file));
        c.version = std::move(source);
        c.defines.push_back("NEXTBUILD_VERSION=\"" + c.version + "\"");
    }
    return c;
}
Paths discover(const fs::path& root) {
    Paths result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator{}; ++it) {
        if (it->is_directory() && (it->path().filename() == ".git" || it->path().filename() == ".build"))
            it.disable_recursion_pending();
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
struct DependencyExport {
    std::size_t provider;
    Variables variables;
    Document items;
};
struct Project {
    Config config;
    std::vector<DependencyExport> exports;
};

class Builder {
public:
    Builder(fs::path root, bool force, bool quiet) : root(std::move(root)), force(force), quiet(quiet) {}

    std::size_t build(const Paths& targets, unsigned jobs, bool show_graph) {
        std::vector<std::size_t> requested;
        for (const auto& target : targets) requested.push_back(plan(target));
        if (show_graph) {
            write_graph();
            return 0;
        }
        const auto project_jobs = std::min<std::size_t>(jobs, graph::maximum_layer(edges));
        const auto compile_jobs = static_cast<unsigned>(jobs / std::max<std::size_t>(1, project_jobs));
        std::vector<Result> results(projects.size());
        graph::execute(edges, static_cast<unsigned>(std::max<std::size_t>(1, project_jobs)), [&](std::size_t index) {
            auto& project = projects[index];
            for (auto& dependency : project.exports) {
                const auto& result = results[dependency.provider];
                auto& vars = dependency.variables;
                vars["DEP_BUILD_ROOT"] = result.output_dir;
                vars["DEP_PCM"] = result.output_dir / "pcm";
                vars["DEP_LIBRARY"] = result.output.empty() ? result.output_dir / "library" : result.output;
                for (const auto& item : dependency.items) {
                    const auto value = absolute(path_value(evaluate(item.expr, vars)), root);
                    append_unique(item.name == "AddModulePath" ? project.config.modules : project.config.libraries, value);
                }
            }
            results[index] = {project.config.output_dir, produce(project.config, root, force, quiet, compile_jobs)};
        });
        merge_databases(results);
        return static_cast<std::size_t>(std::ranges::count_if(requested, [&](std::size_t index) {
            return !results[index].output.empty();
        }));
    }

    void clean(const fs::path& target) {
        const auto c = load_config(project_path(target, root), root);
        if (!fs::exists(c.output_dir)) return;
        const auto dir = fs::canonical(c.output_dir);
        if (dir == dir.root_path() || within(root, dir) || within(c.file, dir) || within(c.source, dir) ||
            std::ranges::any_of(c.extra_sources, [&](const auto& source) { return within(source, dir); }))
            throw std::runtime_error("refusing to clean a directory containing project sources: " + utf8(dir));
        fs::remove_all(dir);
        if (!quiet) LogLine() << "cleaned " << utf8(dir) << '\n';
    }

private:
    fs::path root;
    bool force, quiet;
    std::map<fs::path, std::size_t> indices;
    std::map<fs::path, fs::path> output_dirs;
    std::set<fs::path> active;
    std::vector<Project> projects;
    graph::Edges edges;

    std::size_t plan(const fs::path& target) {
        const auto file = project_path(target, root);
        if (active.contains(file)) throw std::runtime_error("cyclic project dependency: " + utf8(file));
        if (const auto known = indices.find(file); known != indices.end()) return known->second;
        active.insert(file);
        const auto index = projects.size();
        indices.emplace(file, index);
        projects.emplace_back();
        edges.emplace_back();
        Project project{load_config(file, root), {}};
        const auto output = fs::weakly_canonical(project.config.output_dir);
        if (const auto [existing, inserted] = output_dirs.emplace(output, file); !inserted)
            throw std::runtime_error("projects share build directory " + utf8(output) + ": " +
                                     utf8(existing->second) + " and " + utf8(file));
        for (const auto& dependency : project.config.depends) {
            const auto provider = plan(dependency);
            append_unique(edges[index], provider);
        }
        std::set<fs::path> used;
        for (const auto& dependency : project.config.dependencies) use(project, index, dependency, used);
        projects[index] = std::move(project);
        active.erase(file);
        return index;
    }

    void use(Project& project, std::size_t index, const fs::path& file, std::set<fs::path>& used) {
        if (!used.insert(file).second) return;
        auto& c = project.config;
        Variables vars{{"PROJECT_ROOT", root}, {"NEXTOS_MAIN_ROOT", root}};
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
        if (!quiet) LogLine() << "    use " << name << ' ' << version << " (" << utf8(file) << ")\n";
        if (provider.empty()) throw std::runtime_error(utf8(file) + ": RequireProject is required");
        const auto dependency = plan(provider);
        append_unique(edges[index], dependency);
        project.exports.push_back({dependency, std::move(vars), std::move(exports)});
        for (const auto& path : nested) use(project, index, path, used);
    }

    void merge_databases(const std::vector<Result>& results) const {
        const auto root_db = root / "compile_commands.json";
        auto merged = database(root_db);
        for (std::size_t i = 0; i < projects.size(); ++i) {
            if (results[i].output.empty()) continue;
            const auto& config = projects[i].config;
            const auto project_db = config.output_dir / "compile_commands.json";
            if (project_db == root_db) continue;
            std::erase_if(merged, [&](const auto& entry) {
                const auto file = path(entry.first);
                return within(file, config.source) || within(file, config.output_dir / "generated") ||
                    std::ranges::any_of(config.extra_sources, [&](const auto& source) { return within(file, source); });
            });
            for (auto& [file, entry] : database(project_db)) merged.insert_or_assign(file, std::move(entry));
        }
        save_database(root_db, merged);
    }

    void write_graph() const {
        std::ostringstream dot;
        dot << "digraph nextbuild {\n  rankdir=LR;\n";
        for (std::size_t i = 0; i < projects.size(); ++i) {
            const auto& config = projects[i].config;
            const auto project = "p" + std::to_string(i);
            dot << "  " << project << " [shape=box,label=" << Json::quote(config.name + " (link/archive)") << "];\n";
            Paths sources{config.source};
            sources.insert(sources.end(), config.extra_sources.begin(), config.extra_sources.end());
            auto units = collect(sources, config.output_dir, {});
            if (needs_platform_module(config, units)) {
                Unit generated;
                generated.file = config.output_dir / "generated/cmake_generated.cppm";
                generated.module = "cmake_generated";
                generated.bmi = true;
                units.insert(units.begin(), std::move(generated));
            }
            const auto deps = unit_dependencies(units, false);
            const auto unit_id = [&](std::size_t index) { return project + "u" + std::to_string(index); };
            for (std::size_t j = 0; j < units.size(); ++j) {
                const auto node = unit_id(j);
                auto label = utf8(units[j].file.lexically_relative(root));
                if (label.empty()) label = utf8(units[j].file);
                dot << "  " << node << " [label=" << Json::quote(label) << "];\n";
                dot << "  " << node << " -> " << project << ";\n";
                for (const auto dependency : deps[j]) dot << "  " << unit_id(dependency) << " -> " << node << ";\n";
                if (deps[j].empty())
                    for (const auto dependency : edges[i]) dot << "  p" << dependency << " -> " << node << ";\n";
            }
            if (units.empty())
                for (const auto dependency : edges[i]) dot << "  p" << dependency << " -> " << project << ";\n";
        }
        std::cout << dot.str() << "}\n";
    }
};
}

export int cli(const Args& args) {
    auto root = root_from(fs::current_path());
    bool clean = false, force = false, quiet = false, list = false, options = true, show_graph = false;
    auto jobs = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    std::string color_mode = "auto";
    Paths targets;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (options && arg == "--") options = false;
        else if (options && (arg == "--help" || arg == "-h")) {
            std::cout << "Usage: nextbuild [--root DIR] [--list] [--clean] [-B|--force] [-q|--quiet] [-j JOBS] [TARGET ...]\n"
                         "       nextbuild [--root DIR] --graph [TARGET ...]\n"
                         "       nextbuild [--color auto|always|never] [--output on-demand|stream] [TARGET ...]\n"
                         "       nextbuild --version\n"
                         "       nextbuild lsp [--stdio]\n";
            return 0;
        } else if (options && arg == "--version") {
            std::cout << "nextbuild " << nextbuild_version << '\n';
            return 0;
        } else if (options && (arg == "-j" || arg.starts_with("-j") || arg == "--jobs" || arg.starts_with("--jobs="))) {
            std::string_view count;
            if (arg == "-j" || arg == "--jobs") {
                if (++index == args.size()) throw std::runtime_error("expected job count");
                count = args[index];
            } else count = std::string_view(arg).substr(arg.starts_with("--") ? 7 : 2);
            if (count.empty()) throw std::runtime_error("expected job count");
            const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), jobs);
            if (error != std::errc{} || end != count.data() + count.size() || jobs == 0 || jobs > 256)
                throw std::runtime_error("job count must be between 1 and 256");
        } else if (options && (arg == "--color" || arg.starts_with("--color=") ||
                               arg == "--output" || arg.starts_with("--output="))) {
            const bool output = arg.starts_with("--output");
            auto value = arg.find('=');
            std::string mode;
            if (value == std::string::npos) {
                if (++index == args.size()) throw std::runtime_error("expected option value");
                mode = args[index];
            } else mode = arg.substr(value + 1);
            if (output) {
                if (mode != "stream" && mode != "on-demand") throw std::runtime_error("expected stream or on-demand");
                settings.stream = mode == "stream";
            } else {
                if (mode != "auto" && mode != "always" && mode != "never")
                    throw std::runtime_error("expected auto, always or never");
                color_mode = std::move(mode);
            }
        } else if (options && arg == "--graph") show_graph = true;
        else if (options && arg == "--root") {
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
    if (!fs::is_directory(root)) throw std::runtime_error("project root is not a directory: " + utf8(root));
    fs::current_path(root);
    if (list || targets.empty()) targets = discover(root);
    if (list) {
        if (targets.empty()) std::cout << "no projects under " << utf8(root) << '\n';
        for (const auto& target : targets) std::cout << utf8(target.lexically_relative(root)) << '\n';
        return 0;
    }
    if (show_graph && clean) throw std::runtime_error("--graph cannot be combined with --clean");
    const bool use_color = color_mode == "always" || (color_mode == "auto" && !no_color_requested());
    const bool terminal = use_color && terminal_color();
    settings.color = color_mode == "always" || terminal;
    Builder builder(root, force, quiet || show_graph);
    if (clean) {
        for (const auto& target : targets) builder.clean(target);
        return 0;
    }
    if (targets.empty()) throw std::runtime_error("no .nextbuild files found under " + utf8(root));
    const auto built = builder.build(targets, jobs, show_graph);
    if (!quiet && !show_graph) std::cout << "nextbuild: finished " << built << '/' << targets.size() << " project(s)\n";
    return 0;
}
}
