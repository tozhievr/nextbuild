# Manifest reference

**English** · [Русский](manifest.ru.md) · [README](../README.md)

A manifest is a UTF-8 text file. Directives and variables use `name : value`. Directive names are case-sensitive. Comments start with `#` or `//`.

```text
Project : Example
Version : 1.2.0
@Sources : @PROJECT_ROOT / 'source tree'
SetSource : @Sources
EnableCompileFlag : '-Wall'
EnableCompileFlag : '-Iinclude'
```

Values include quoted strings, identifiers, booleans, decimal and hexadecimal numbers, dotted versions, and blocks. Use `/` to join path segments. Relative paths are resolved from `@PROJECT_ROOT`. Define variables before using them.

## Project directives

| Directive | Repeatable | Description |
| --- | --- | --- |
| `Project` | — | Display name and default output basename |
| `Version` | — | Project version shown in build output |
| `SetVersionFile` | — | Reads a version file and defines `NEXTBUILD_VERSION` |
| `SetSource` | — | Primary source file or directory |
| `AddSource` | Yes | Additional source file or directory |
| `SetBuildRoot` | — | Output directory, defaults to `.build/<name>` |
| `SetOutputName` | — | Overrides the artifact basename |
| `SetProductKind` | — | `host`, `library`, or `nxflat` |
| `SetLibraryKind` | — | `host` or `freestanding` |
| `SetExecutableLayout` | — | Label and address pairs for NXFL |
| `SetTargetTriple` | — | Clang target triple |
| `SetCompiler` | — | Clang-compatible executable or full path |
| `SetMinCompilerVer` | — | Required minimum compiler version |
| `SetStandard` | — | C++ standard, defaults to `c++20`. C inputs use C23 |
| `EnableCompileFlag` | Yes | One compiler argument |
| `EnableLinkFlag` | Yes | One linker argument |
| `AddDefine` | Yes | Preprocessor definition without `-D` |
| `AddModulePath` | Yes | Directory containing compatible prebuilt BMIs |
| `LinkLibrary` | Yes | Path to a library to link |
| `Depend` | Yes | Build another project first |
| `UseDependency` | Yes | Import a `.nextbuild-dependency` export |
| `SetNextBuildDrawProgress` | — | Show a progress bar when `true` |
| `SetGeneratePlatformModule` | — | Generate `cmake_generated` for a library |
| `EnableFeature` | Yes | Enable a feature tag in `cmake_generated` |

## Products

A project produces a native executable by default. Use `SetProductKind : library` for a static archive. The `freestanding` library profile defaults to `x86_64-unknown-none-elf`.

```text
SetProductKind : nxflat
SetExecutableLayout : {
    - start = 0x10000
}
EnableLinkFlag : '-Wl,--gc-sections'
```

NXFL links an intermediate x86-64 ELF. The linker places sections such as `.start` and `.text.start` at the specified addresses and records the map in `.labels.json`. In this example `start` is also the entry symbol.

## Dependency exports

Add `UseDependency : @PROJECT_ROOT / libs / core / '.nextbuild-dependency'` to the consumer and create the referenced file.

```text
Dependency : Core
RequireProject : @PROJECT_ROOT / libs / core
AddModulePath : @DEP_PCM
LinkLibrary : @DEP_LIBRARY
```

`RequireProject` identifies the provider. Exported paths can use `@DEP_BUILD_ROOT`, `@DEP_PCM` and `@DEP_LIBRARY`. A dependency export can also use `Version`, `LibraryKind`, `EnableCompileFlag`, `EnableLinkFlag`, `AddDefine` and nested `UseDependency`.

## Outputs and diagnostics

Nextbuild writes `compile_commands.json` into the build directory and updates the merged database in the workspace root. Changes to commands, headers or BMIs invalidate affected objects. Run without `-q` for the build commands and compiler errors.

If a BMI is incompatible, rebuild the provider and consumer using the same Clang, C++ standard and target. If editor features are unavailable, check `nextbuild.executablePath` and the **Nextbuild** Output channel.
