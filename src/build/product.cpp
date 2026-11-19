#include "process.hpp"
#include "config.hpp"
#include "json.hpp"
#include <algorithm>
#include <sstream>

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
