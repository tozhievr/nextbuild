# Nextbuild

[![Build and test](https://github.com/tozhievr/nextbuild/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/tozhievr/nextbuild/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/tozhievr/nextbuild)](https://github.com/tozhievr/nextbuild/releases/latest)
[![License](https://img.shields.io/github/license/tozhievr/nextbuild)](LICENSE)

**English** · [Русский](README.ru.md)

Nextbuild builds C and C++ projects from `.nextbuild` manifests. It orders
named C++ modules, tracks header and BMI dependencies, and produces executables,
static libraries and freestanding NXFL images. The same binary runs the
language server for VS Code and Cursor.

[Install](#install) · [Quick start](#quick-start) · [Editor support](#editor-support) ·
[Manifest reference](docs/manifest.md) · [Contributing](CONTRIBUTING.md)

## Install

Published binaries cover **Windows x64**, **Linux x86-64**, and **macOS
Apple Silicon**. The installers select the latest stable release and verify
the archive against its published SHA-256 checksum. They install for the
current user with no administrator privileges.

**Windows PowerShell**

```powershell
& ([scriptblock]::Create((Invoke-RestMethod 'https://raw.githubusercontent.com/tozhievr/nextbuild/main/install.ps1')))
```

**Windows Command Prompt**

```bat
curl.exe -fsSL "https://raw.githubusercontent.com/tozhievr/nextbuild/main/instal.bat" -o "%TEMP%\nextbuild-install.bat" && call "%TEMP%\nextbuild-install.bat"
```

**Linux and macOS**

```sh
( tmp=$(mktemp) && trap 'rm -f "$tmp"' EXIT && curl -fsSL 'https://raw.githubusercontent.com/tozhievr/nextbuild/main/install.sh' -o "$tmp" && sh "$tmp" )
```

Windows installs into `%LOCALAPPDATA%\Nextbuild\bin` and adds that directory
to the user PATH. Open a new Command Prompt or terminal if it was already
running. On Linux and macOS the default is `$HOME/.local/bin`. If needed, add
`export PATH="$HOME/.local/bin:$PATH"` to your shell startup file.

Check the installation with `nextbuild --version`. Run the same command to
upgrade. To install a particular release or inspect the script first, save
the installer and run one of these commands:

```powershell
.\install.ps1 -Version 0.2.1 -BinDir "$HOME\bin" -NoPath
```

```bat
instal.bat -Version 0.2.1 -BinDir "%USERPROFILE%\bin" -NoPath
```

```sh
sh install.sh --version 0.2.1 --bin-dir "$HOME/bin"
```

You can also [download an archive](https://github.com/tozhievr/nextbuild/releases/latest),
check it against `SHA256SUMS`, and extract the executable and license notice.
To uninstall, remove `nextbuild` (or `nextbuild.exe`) and
`nextbuild-Parsley-LICENSE` from the install directory. Remove its PATH entry
if you added that directory exclusively for Nextbuild.

## Quick start

Create `.nextbuild` in the project root:

```text
Project : Hello
SetSource : @PROJECT_ROOT / src
SetStandard : c++23
```

Create `src/main.cpp`:

```cpp
#include <iostream>

int main() {
    std::cout << "Hello from Nextbuild\n";
}
```

Run `nextbuild` from the project root. The result is
`.build/hello/hello` or `.build/hello/hello.exe` on Windows.
The runtime package contains the build tool and language server.
Install Clang to compile projects. Static libraries also require `llvm-ar`
or `ar`, and NXFL requires `ld.lld` and `llvm-objcopy`.

| Command | What it does |
| --- | --- |
| `nextbuild --list` | Lists manifests under the workspace root |
| `nextbuild apps/hello` | Builds a directory or a specific `.nextbuild` file |
| `nextbuild -B apps/hello` | Forces recompilation |
| `nextbuild --clean apps/hello` | Removes the project's build outputs |
| `nextbuild --root DIR --list` | Uses an explicit workspace root |
| `nextbuild -q` | Hides routine build output |

Use `nextbuild -j 8` for a parallel build or `nextbuild --graph > build.dot`
to export the dependency graph. See [parallel builds and System Runtime](docs/runtime.md)
for output modes, colors and the subprocess API.

See the [manifest reference](docs/manifest.md) for product types, dependency
exports, compile flags and NXFL layouts.

## Editor support

Download the VSIX from the [latest release](https://github.com/tozhievr/nextbuild/releases/latest).
Run **Extensions: Install from VSIX…** in VS Code or Cursor and open a
Nextbuild manifest in a trusted workspace. The extension provides
highlighting, completion, diagnostics, hover, definitions and document
symbols. The command palette also exposes **Nextbuild: Build** and
**Nextbuild: Clean**.

The extension starts `nextbuild lsp --stdio`. If the editor cannot find the
executable, restart the editor after updating PATH or set
`nextbuild.executablePath` to the full path to the binary.
[Extension documentation](extension/README.md)

## Build from source

Use CMake 3.30+, Ninja and LLVM Clang 22+ with `clang-scan-deps`. On Windows
use Developer PowerShell for Visual Studio with the Windows SDK. On macOS
use upstream LLVM and the Xcode Command Line Tools.

```sh
git clone https://github.com/tozhievr/nextbuild.git
cd nextbuild
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix build/install
```

Use an installed binary or the CMake bootstrap to build Nextbuild with
its own manifest:

```sh
nextbuild --root . .nextbuild
```

The self-built result is in `.build/nextbuild/`. Changes to the grammar and
PSL use the [Parsley workflow](CONTRIBUTING.md#manifest-parser-and-semantics).
Releases are driven by [VERSION](VERSION) and built on all three platforms.

## License

[MIT](LICENSE). The generated Parsley parser is distributed with its
[license notice](generated/manifest/LICENSE).
