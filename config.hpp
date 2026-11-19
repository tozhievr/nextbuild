#pragma once
#include "nextbuild.hpp"

namespace nb {
inline const Args project_directives{
    "Project", "Version", "SetBuildRoot", "SetSource", "SetStandard", "SetCompiler",
    "SetMinCompilerVer", "SetNextBuildDrawProgress", "EnableCompileFlag", "EnableLinkFlag",
    "AddModulePath", "AddDefine", "SetOutputName", "SetTargetTriple", "SetExecutableLayout",
    "SetProductKind", "SetLibraryKind", "EnableFeature", "UseDependency", "LinkLibrary", "Depend"
};
inline const Args dependency_directives{
    "Dependency", "Version", "LibraryKind", "RequireProject", "AddModulePath", "LinkLibrary",
    "EnableCompileFlag", "EnableLinkFlag", "AddDefine", "UseDependency"
};
using Paths = std::vector<fs::path>;
struct Config {
    fs::path file, source, output_dir;
    std::string name = "Untitled", version = "0.0.0", output_name;
    std::string product, library, compiler = "clang++", minimum, standard = "c++20", triple;
    bool progress = false;
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
