module;
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

namespace nb {
using Paths = std::vector<fs::path>;
struct Config {
    fs::path file, source, output_dir;
    std::string name = "Untitled", version = "0.0.0", output_name;
    std::string product, library, compiler = "clang++", minimum, standard = "c++20", triple;
    bool progress = false, platform_module = false;
    Args flags, link_flags, defines, features;
    Paths modules, libraries, depends, dependencies;
    Layout layout;
};
fs::path absolute(fs::path value, const fs::path& root);
Document load_document(const fs::path& file);
void require_directive(const Statement& item, const Args& allowed, const fs::path& file);
Config load_config(const fs::path& file, const fs::path& root);
Paths discover(const fs::path& root);
fs::path project_path(const fs::path& target, const fs::path& root);
fs::path produce(Config& c, const fs::path& root, bool force, bool quiet);
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
    std::cout << "    " << stage;
    for (std::size_t i = 1; i < args.size(); ++i) std::cout << ' ' << args[i];
    std::cout << '\n';
}

fs::path produce(Config& c, const fs::path& root, bool force, bool quiet) {
    if (c.product.empty()) {
        if (!c.layout.empty()) c.product = "nxflat";
        else if (!c.library.empty()) c.product = "library";
        else c.product = "host";
    }
    if (c.product == "nxflat" && c.layout.empty())
        throw std::runtime_error("SetExecutableLayout is required for nxflat products");
    if (c.product == "library" && c.library.empty()) c.library = "host";
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
    fs::path generated;
    auto units = collect(c.source, output_dir, {});
    const bool imports_platform = std::ranges::any_of(units, [](const Unit& unit) {
        return std::ranges::find(unit.imports, "cmake_generated") != unit.imports.end();
    });
    if (c.product == "library" && (c.platform_module || !c.features.empty() || imports_platform)) {
        generated = generate(c);
        units = collect(c.source, output_dir, generated);
    }
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
        const bool track_headers = !unit.assembly || unit.file.extension() == ".S";
        auto depfile = object;
        depfile += ".d";
        if (track_headers) args.insert(args.end(), {"-MD", "-MF", utf8(depfile), "-MT", "nextbuild-object"});
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
        const auto headers = track_headers ? read_dependencies(depfile) : std::nullopt;
        if (headers) inputs.insert(inputs.end(), headers->begin(), headers->end());
        const bool dirty = force || (track_headers && !headers) || previous == old.end() ||
            previous->second["arguments"].dump() != Json(arguments).dump() ||
            outdated(object, inputs) || (unit.bmi && outdated(pcm, inputs));
        if (dirty) {
            print_command("cm", args, !quiet && !c.progress);
            fs::remove(object);
            if (unit.bmi) fs::remove(pcm);
            if (track_headers) fs::remove(depfile);
            try { run(args); }
            catch (...) {
                fs::remove(object);
                if (unit.bmi) fs::remove(pcm);
                if (track_headers) fs::remove(depfile);
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
        run({utf8(objcopy), "-O", "binary", "-j", "." + name, utf8(intermediate), utf8(blob)});
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
    try { run(args); }
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
        args.insert(args.end(), {"-nostdlib", "-static", "-fuse-ld=lld", "-Xlinker", "-T",
                                "-Xlinker", utf8(script), "-Wl,--build-id=none", "-Wl,--no-dynamic-linker"});
        output = dir / path(c.output_name + ".nxflat");
    }
    args.insert(args.end(), c.link_flags.begin(), c.link_flags.end());
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

export int cli(const Args& args) {
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
    if (!fs::is_directory(root)) throw std::runtime_error("project root is not a directory: " + utf8(root));
    fs::current_path(root);
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
