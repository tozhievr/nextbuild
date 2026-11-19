from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class DirectiveInfo:
    name: str
    summary: str
    value_kind: str
    detail: str
    examples: tuple[str, ...] = ()


DIRECTIVES: dict[str, DirectiveInfo] = {
    "Project": DirectiveInfo(
        name="Project",
        summary="Human-readable project name",
        value_kind="ident",
        detail="Sets the project display name. Also used as the default output basename.",
        examples=("Project : NextBoot",),
    ),
    "Version": DirectiveInfo(
        name="Version",
        summary="Project version",
        value_kind="version",
        detail="Dotted version string stored in build metadata.",
        examples=("Version : 0.1.0",),
    ),
    "SetBuildRoot": DirectiveInfo(
        name="SetBuildRoot",
        summary="Build output directory",
        value_kind="path",
        detail="Directory for pcm/obj/segments and the final `.nxflat` image.",
        examples=("SetBuildRoot : @NEXTOS_MAIN_ROOT / '.build' / 'bootloader'",),
    ),
    "SetSource": DirectiveInfo(
        name="SetSource",
        summary="Source tree root",
        value_kind="path",
        detail="Root scanned for C++20 modules (`.cppm`, `.ixx`, …) and optional asm.",
        examples=("SetSource : @NEXTOS_MAIN_ROOT / 'bootloader'",),
    ),
    "SetStandard": DirectiveInfo(
        name="SetStandard",
        summary="C++ language standard",
        value_kind="ident",
        detail="Passed to the compiler as `-std=<value>`. Prefer `c++20` / `c++2d`.",
        examples=("SetStandard : c++2d", "SetStandard : c++20"),
    ),
    "SetCompiler": DirectiveInfo(
        name="SetCompiler",
        summary="Compiler executable",
        value_kind="ident",
        detail="Compiler looked up on PATH (typically `clang++`).",
        examples=("SetCompiler : clang++",),
    ),
    "SetMinCompilerVer": DirectiveInfo(
        name="SetMinCompilerVer",
        summary="Minimum compiler version",
        value_kind="version",
        detail="Build fails if `clang++ --version` is older than this dotted version.",
        examples=("SetMinCompilerVer : 23.1.3",),
    ),
    "SetNextBuildDrawProgress": DirectiveInfo(
        name="SetNextBuildDrawProgress",
        summary="Draw progress bar",
        value_kind="bool",
        detail="When `True`, nextbuild draws a progress bar while compiling modules.",
        examples=("SetNextBuildDrawProgress : True",),
    ),
    "EnableCompileFlag": DirectiveInfo(
        name="EnableCompileFlag",
        summary="Add a compile flag",
        value_kind="flag",
        detail="Appends one flag to the module compile command. Repeatable.",
        examples=(
            "EnableCompileFlag : '-ffreestanding'",
            "EnableCompileFlag : '-fno-exceptions'",
        ),
    ),
    "EnableLinkFlag": DirectiveInfo(
        name="EnableLinkFlag",
        summary="Add a link flag",
        value_kind="flag",
        detail="Appends one flag to the intermediate link step. Repeatable.",
        examples=("EnableLinkFlag : '-Wl,-z,max-page-size=0x1000'",),
    ),
    "AddModulePath": DirectiveInfo(
        name="AddModulePath",
        summary="Extra BMI/module search path",
        value_kind="path",
        detail="Adds `-fprebuilt-module-path=` for consuming prebuilt `.pcm` files.",
        examples=("AddModulePath : @NEXTOS_MAIN_ROOT / '.build' / 'stdlib' / 'pcm'",),
    ),
    "AddDefine": DirectiveInfo(
        name="AddDefine",
        summary="Preprocessor define",
        value_kind="ident",
        detail="Adds `-DNAME` (or `-DNAME=value` if provided as a string).",
        examples=("AddDefine : NEXTOS_BOOT", "AddDefine : 'NEXTOS_DEBUG=1'"),
    ),
    "SetOutputName": DirectiveInfo(
        name="SetOutputName",
        summary="Output basename",
        value_kind="ident",
        detail="Overrides the default basename derived from `Project` (without extension).",
        examples=("SetOutputName : nextboot",),
    ),
    "SetTargetTriple": DirectiveInfo(
        name="SetTargetTriple",
        summary="Clang target triple",
        value_kind="ident",
        detail=(
            "Optional. Defaults to `x86_64-unknown-none-elf` for freestanding "
            "intermediate objects. Final ship format is still NXFL."
        ),
        examples=("SetTargetTriple : x86_64-unknown-none-elf",),
    ),
    "SetExecutableLayout": DirectiveInfo(
        name="SetExecutableLayout",
        summary="Named segments and load addresses",
        value_kind="layout",
        detail=(
            "Required for `SetProductKind : nxflat`. Declares labeled segments "
            "for the NXFL flat image."
        ),
        examples=(
            "SetExecutableLayout : {\n    - start = 0x10000\n    - main  = 0x12000\n}",
        ),
    ),
    "SetProductKind": DirectiveInfo(
        name="SetProductKind",
        summary="Product type: nxflat | library | host",
        value_kind="ident",
        detail=(
            "`nxflat` — freestanding labeled flat image (OS). "
            "`library` — static library + BMI modules. "
            "`host` — Windows/host executable (e.g. VM)."
        ),
        examples=(
            "SetProductKind : library",
            "SetProductKind : host",
            "SetProductKind : nxflat",
        ),
    ),
    "SetLibraryKind": DirectiveInfo(
        name="SetLibraryKind",
        summary="Library target profile: host | next",
        value_kind="ident",
        detail=(
            "`host` — host stdlib (Win64, cmake_generated, COFF). "
            "`next` — freestanding NextOS stdlib (ELF intermediates)."
        ),
        examples=("SetLibraryKind : host", "SetLibraryKind : next"),
    ),
    "EnableFeature": DirectiveInfo(
        name="EnableFeature",
        summary="Enable a cmake_generated feature tag",
        value_kind="ident",
        detail="Repeatable. Host defaults include AVX2 and FMA. Also: SSE42, AVX512, AVX512BW, VNNI.",
        examples=("EnableFeature : AVX2", "EnableFeature : FMA"),
    ),
    "UseDependency": DirectiveInfo(
        name="UseDependency",
        summary="Consume a .nextbuild-dependency export",
        value_kind="path",
        detail=(
            "Path to a `.nextbuild-dependency` file. Builds `RequireProject` first, "
            "then injects module paths, libraries and flags into this project."
        ),
        examples=(
            "UseDependency : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host' / '.nextbuild-dependency'",
        ),
    ),
    "LinkLibrary": DirectiveInfo(
        name="LinkLibrary",
        summary="Link an extra static library",
        value_kind="path",
        detail="Repeatable absolute/relative library path passed to the final link step.",
        examples=("LinkLibrary : @NEXTOS_MAIN_ROOT / '.build' / 'stdlib-host' / 'libcpp26host.lib'",),
    ),
    "Depend": DirectiveInfo(
        name="Depend",
        summary="Build another nextbuild project first",
        value_kind="path",
        detail="Path to a project directory or `.nextbuild` file that must be built first.",
        examples=("Depend : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host'",),
    ),
}

PATH_DIRECTIVES = {name for name, info in DIRECTIVES.items() if info.value_kind == "path"}
BOOL_DIRECTIVES = {name for name, info in DIRECTIVES.items() if info.value_kind == "bool"}
LAYOUT_DIRECTIVES = {name for name, info in DIRECTIVES.items() if info.value_kind == "layout"}

BUILTIN_VARIABLES: dict[str, str] = {
    "NEXTOS_MAIN_ROOT": "Absolute path to the NextOS repository root (auto-provided by nextbuild).",
    "DEP_BUILD_ROOT": "Provider build root (only inside `.nextbuild-dependency`).",
    "DEP_PCM": "Provider BMI directory (`build_root/pcm`).",
    "DEP_LIBRARY": "Provider static library path.",
}

PRODUCT_KIND_SUGGESTIONS = ("nxflat", "library", "host")
LIBRARY_KIND_SUGGESTIONS = ("host", "next")
FEATURE_SUGGESTIONS = ("SSE42", "AVX2", "FMA", "AVX512", "AVX512BW", "VNNI")

STANDARD_SUGGESTIONS = ("c++20", "c++23", "c++2b", "c++2c", "c++2d", "c++26")
COMPILER_SUGGESTIONS = ("clang++", "clang")
COMMON_COMPILE_FLAGS = (
    "'-ffreestanding'",
    "'-fno-exceptions'",
    "'-fno-rtti'",
    "'-fno-threadsafe-statics'",
    "'-nostdinc++'",
    "'-nobuiltininc'",
)

DEPENDENCY_DIRECTIVES: dict[str, DirectiveInfo] = {
    "Dependency": DirectiveInfo(
        name="Dependency",
        summary="Dependency export name",
        value_kind="ident",
        detail="Human-readable name of this dependency package.",
        examples=("Dependency : LibCpp26Host",),
    ),
    "Version": DIRECTIVES["Version"],
    "LibraryKind": DirectiveInfo(
        name="LibraryKind",
        summary="host | next",
        value_kind="ident",
        detail="Library profile this dependency provides.",
        examples=("LibraryKind : host",),
    ),
    "RequireProject": DirectiveInfo(
        name="RequireProject",
        summary="Provider .nextbuild project to build first",
        value_kind="path",
        detail="Path to the library project directory or `.nextbuild` file.",
        examples=("RequireProject : @NEXTOS_MAIN_ROOT / 'stdlib' / 'host'",),
    ),
    "AddModulePath": DIRECTIVES["AddModulePath"],
    "LinkLibrary": DIRECTIVES["LinkLibrary"],
    "EnableCompileFlag": DIRECTIVES["EnableCompileFlag"],
    "EnableLinkFlag": DIRECTIVES["EnableLinkFlag"],
    "AddDefine": DIRECTIVES["AddDefine"],
}

DEPENDENCY_PATH_DIRECTIVES = {
    name for name, info in DEPENDENCY_DIRECTIVES.items() if info.value_kind == "path"
}

DEP_BUILTIN_VARIABLES: dict[str, str] = {
    "NEXTOS_MAIN_ROOT": BUILTIN_VARIABLES["NEXTOS_MAIN_ROOT"],
    "DEP_BUILD_ROOT": BUILTIN_VARIABLES["DEP_BUILD_ROOT"],
    "DEP_PCM": BUILTIN_VARIABLES["DEP_PCM"],
    "DEP_LIBRARY": BUILTIN_VARIABLES["DEP_LIBRARY"],
}


def is_dependency_document(path: str | None) -> bool:
    if not path:
        return False
    name = Path(path).name.lower()
    return name == ".nextbuild-dependency" or name.endswith(".nextbuild-dependency")


def directive_markdown(info: DirectiveInfo) -> str:
    lines = [
        f"**`{info.name}`** — {info.summary}",
        "",
        info.detail,
        "",
        f"_Value kind:_ `{info.value_kind}`",
    ]
    if info.examples:
        lines.append("")
        lines.append("```nextbuild")
        lines.extend(info.examples)
        lines.append("```")
    return "\n".join(lines)
