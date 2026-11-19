#pragma once
#include <map>
#include <string>

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
