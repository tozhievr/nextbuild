# Contributing to Nextbuild

The [source build instructions](README.md#build-from-source) cover the bootstrap.
Run CTest, then build Nextbuild with its own manifest to exercise the second
generation. Keep the bootstrap outside the self-build output directory so
Windows can replace the resulting executable.

```sh
ctest --test-dir build --output-on-failure
nextbuild --root . .nextbuild
```

## Project layout

| Path | Contents |
| --- | --- |
| `src/app` | CLI entry point |
| `src/build` | Project loading, compilation, linking, and packaging |
| `src/manifest` | C++ adapter for the generated manifest parser and semantics |
| `src/lsp` | Language server and directive metadata |
| `src/platform` | Native process adapters |
| `grammar` | Parsley grammar and PSL semantic rules |
| `generated/manifest` | Checked-in C23 output of Parsley |
| `extension` | VS Code and Cursor extension |

## Manifest parser and semantics

Use Parsley **2.35.1**. Update both `grammar/NextbuildManifest.parsley` and
`grammar/NextbuildManifest.psem` when the syntax and semantics change.

```sh
cmake -S . -B build -DPARSLEY_EXECUTABLE=/path/to/parsley
cmake --build build --target manifest-regenerate
cmake --build build --target manifest-check
ctest --test-dir build --output-on-failure
```

Commit the grammar, PSL, generated files and affected editor tests together.

## Extension

Use Node.js 22 or newer.

```sh
node tools/version.mjs verify
npm ci --ignore-scripts --prefix extension
cd extension
npx --yes @vscode/vsce@3.7.1 package
```

## Releases

The root `VERSION` is the source of truth. Bump it with
`node tools/version.mjs bump patch` (or `minor` / `major`), verify the
synchronized metadata with `node tools/version.mjs verify`, and commit
the version files together. The GitHub Actions release workflow triggers
when the new `VERSION` reaches `main`. It builds with Nextbuild on all
three platforms, then publishes the annotated tag, archives, VSIX and
`SHA256SUMS`.

You can also run **Release Nextbuild** manually and select a bump type.
The workflow commits it to `main` when branch permissions permit. Choose
`none` to retry publishing the current version.
