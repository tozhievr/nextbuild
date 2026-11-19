from __future__ import annotations

import os
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from .compile_db import entry_for, merge_compile_commands, write_compile_commands
from .compiler import ensure_min_version, find_tool, probe_compiler, run_command
from .config import ProjectConfig, load_project
from .dependency import load_dependency, resolve_dependency_exports
from .errors import BuildError
from .features import DEFAULT_HOST_FEATURES, write_cmake_generated
from .flat import (
    pack_labeled_flat,
    write_labels_sidecar,
    write_nxfl,
    write_section_script,
)
from .modules import (
    ASM_SUFFIXES,
    ModuleUnit,
    UnitKind,
    collect_units,
    order_units,
    pcm_filename,
    scan_unit,
)
from .progress import ProgressBar

DEFAULT_FREESTANDING_TRIPLE = "x86_64-unknown-none-elf"


@dataclass
class BuildResult:
    config: ProjectConfig
    output: Path | None
    labels_path: Path | None = None
    compile_commands: Path | None = None
    objects: list[Path] = field(default_factory=list)
    compiled: int = 0
    skipped: int = 0
    product_kind: str = "nxflat"


class Builder:
    def __init__(
        self,
        *,
        main_root: Path,
        jobs: int = 1,
        force: bool = False,
        quiet: bool = False,
        builtins: dict[str, Any] | None = None,
    ):
        self.main_root = main_root.resolve()
        self.jobs = max(1, jobs)
        self.force = force
        self.quiet = quiet
        self.builtins = builtins or {}
        self._built: dict[Path, BuildResult] = {}

    def load(self, nextbuild_path: Path) -> ProjectConfig:
        return load_project(nextbuild_path, main_root=self.main_root, builtins=self.builtins)

    def build_path(self, target: Path) -> BuildResult:
        nextbuild = resolve_nextbuild_path(target, self.main_root)
        key = nextbuild.resolve()
        if key in self._built:
            return self._built[key]

        config = self.load(key)

        for dependency in config.depends:
            self.build_path(dependency)

        for dep_file in config.use_dependencies:
            self._apply_use_dependency(config, dep_file)

        result = self._build_project(config)
        self._built[key] = result
        return result

    def build_all(self) -> list[BuildResult]:
        projects = discover_projects(self.main_root)
        if not projects:
            raise BuildError(f"no .nextbuild files found under {self.main_root}")
        return [self.build_path(path) for path in projects]

    def clean_path(self, target: Path) -> Path | None:
        config = self.load(resolve_nextbuild_path(target, self.main_root))
        if config.build_root and config.build_root.exists():
            import shutil

            shutil.rmtree(config.build_root)
            self._log(f"cleaned {config.build_root}")
            return config.build_root
        return None

    def _apply_use_dependency(
        self,
        config: ProjectConfig,
        dep_file: Path,
        seen: set[Path] | None = None,
    ) -> None:
        if seen is None:
            seen = set()
        key = Path(dep_file).resolve()
        if key in seen:
            return
        seen.add(key)
        spec = load_dependency(dep_file, main_root=self.main_root, builtins=self.builtins)
        self._log(f"    use {spec.name} {spec.version} ({dep_file})")

        if spec.require_project is None:
            raise BuildError(f"{dep_file}: RequireProject is required")

        provider = self.build_path(spec.require_project)
        if provider.config.build_root is None:
            raise BuildError(f"{dep_file}: provider has no build root")
        if provider.output is None and provider.product_kind == "library":
            raise BuildError(f"{dep_file}: provider library was not produced")

        modules, libraries = resolve_dependency_exports(
            spec,
            main_root=self.main_root,
            build_root=provider.config.build_root,
            library_path=provider.output,
        )
        for path in modules:
            if path not in config.module_paths:
                config.module_paths.append(path)
        for path in libraries:
            if path not in config.link_libraries:
                config.link_libraries.append(path)
        for flag in spec.compile_flags:
            if flag not in config.compile_flags:
                config.compile_flags.append(flag)
        for flag in spec.link_flags:
            if flag not in config.link_flags:
                config.link_flags.append(flag)
        for define in spec.defines:
            if define not in config.defines:
                config.defines.append(define)

        if spec.library_kind and not config.library_kind:
            # Soft hint only — consumer product kind stays authoritative.
            pass

        for nested in spec.use_dependencies:
            self._apply_use_dependency(config, nested, seen)

    def _build_project(self, config: ProjectConfig) -> BuildResult:
        assert config.build_root is not None
        assert config.source is not None

        product = config.resolved_product_kind()
        self._log(f"==> building {config.project} {config.version} [{product}]")
        self._log(f"    source : {config.source}")
        self._log(f"    output : {config.build_root}")

        if product == "nxflat" and not config.executable_layout:
            raise BuildError(f"{config.path}: SetExecutableLayout is required for nxflat products")
        if product == "library" and not config.library_kind:
            raise BuildError(f"{config.path}: SetLibraryKind (host|next) is required for libraries")

        info = probe_compiler(config.compiler)
        ensure_min_version(info, config.min_compiler_ver)
        self._log(f"    compiler: {info.binary} ({info.version_text})")

        build_root = config.build_root
        obj_root = build_root / "obj"
        pcm_root = build_root / "pcm"
        gen_root = build_root / "generated"
        for path in (build_root, obj_root, pcm_root, gen_root):
            path.mkdir(parents=True, exist_ok=True)

        units = order_units(collect_units(config.source))

        # Host / next libraries synthesize cmake_generated for adaptive layers.
        if product == "library":
            features = set(config.features) or (
                set(DEFAULT_HOST_FEATURES) if config.library_kind == "host" else set()
            )
            generated = write_cmake_generated(
                gen_root / "cmake_generated.cppm",
                library_kind=config.library_kind or "next",
                features=features,
            )
            units = order_units([scan_unit(generated), *units])

        if not units:
            self._log("    warning: no module/asm sources found")
            return BuildResult(config=config, output=None, product_kind=product)

        progress = ProgressBar(
            total=len(units) + 1,
            enabled=config.draw_progress and not self.quiet,
            label=config.project,
        )

        triple = self._select_triple(config, product)
        objects: list[Path] = []
        compiled = 0
        skipped = 0
        pcm_by_module: dict[str, Path] = {}
        compile_entries: list[dict] = []
        # A changed compiler command invalidates cached objects even if source
        # and BMI timestamps are older. In particular this prevents objects
        # built with a formerly shadowed dependency BMI from being reused.
        old_commands: dict[str, list[str]] = {}
        previous_db = build_root / "compile_commands.json"
        if previous_db.is_file():
            try:
                previous = json.loads(previous_db.read_text(encoding="utf-8"))
                if isinstance(previous, list):
                    old_commands = {
                        item["file"]: item["arguments"]
                        for item in previous
                        if isinstance(item, dict)
                        and isinstance(item.get("file"), str)
                        and isinstance(item.get("arguments"), list)
                    }
            except (OSError, ValueError):
                pass

        for unit in units:
            object_path = object_path_for(unit.path, config.source, obj_root)
            # Generated sources live outside SetSource — keep them under obj/generated.
            if gen_root in unit.path.parents or unit.path.parent == gen_root:
                object_path = obj_root / "generated" / (unit.path.stem + ".o")
            object_path.parent.mkdir(parents=True, exist_ok=True)
            pcm_path = pcm_root / pcm_filename(unit) if unit.produces_bmi else None

            argv = self._compile_argv(
                config,
                info.binary,
                unit,
                object_path,
                pcm_path=pcm_path,
                pcm_root=pcm_root,
                pcm_by_module=pcm_by_module,
                triple=triple,
            )
            compile_entries.append(
                entry_for(directory=self.main_root, source=unit.path, arguments=argv)
            )

            outputs = [object_path]
            if pcm_path is not None:
                outputs.append(pcm_path)

            # A BMI contains imported layouts and inline definitions. Reusing an
            # object after an imported BMI changes can silently mix incompatible
            # ABIs, even when the importing source itself has not changed.
            bmi_inputs = list(pcm_by_module.values())
            for module_path in config.module_paths:
                bmi_inputs.extend(module_path.glob("*.pcm"))
            oldest_output = min((p.stat().st_mtime_ns for p in outputs if p.exists()), default=0)
            imports_changed = any(p.stat().st_mtime_ns > oldest_output for p in bmi_inputs)
            command_changed = old_commands.get(str(unit.path.resolve())) != argv
            if self._needs_rebuild(unit.path, outputs) or imports_changed or command_changed:
                if not config.draw_progress:
                    self._log("    cm " + " ".join(argv[1:]))
                run_command(argv)
                compiled += 1
                progress.update(status=unit.path.name)
            else:
                skipped += 1
                progress.update(status=f"{unit.path.name} (cached)")

            if pcm_path is not None and unit.module and unit.kind is UnitKind.INTERFACE:
                pcm_by_module[unit.module] = pcm_path
            elif pcm_path is not None and unit.produces_bmi:
                pcm_by_module[unit.logical_name] = pcm_path

            objects.append(object_path)

        project_db = write_compile_commands(build_root / "compile_commands.json", compile_entries)
        root_db = merge_compile_commands(
            self.main_root / "compile_commands.json",
            compile_entries,
            replace_under=config.source,
        )
        self._log(f"    compile_commands: {project_db}")
        if root_db != project_db:
            self._log(f"    compile_commands: {root_db}")

        if product == "library":
            output = self._archive_library(config, info, objects, build_root)
            progress.update(status=output.name)
            progress.finish("ok")
            self._log(f"    lib {output} ({compiled} compiled, {skipped} cached)")
            return BuildResult(
                config=config,
                output=output,
                compile_commands=root_db,
                objects=objects,
                compiled=compiled,
                skipped=skipped,
                product_kind=product,
            )

        if product == "host":
            output = self._link_host(config, info, objects, build_root, triple=triple)
            progress.update(status=output.name)
            progress.finish("ok")
            self._log(f"    host {output} ({compiled} compiled, {skipped} cached)")
            return BuildResult(
                config=config,
                output=output,
                compile_commands=root_db,
                objects=objects,
                compiled=compiled,
                skipped=skipped,
                product_kind=product,
            )

        # nxflat
        seg_root = build_root / "segments"
        seg_root.mkdir(parents=True, exist_ok=True)
        link_script = build_root / "sections.ld"
        write_section_script(link_script, config.executable_layout)
        intermediate = build_root / "intermediate.elf"
        output = build_root / f"{config.output_basename}.nxflat"
        labels_path = build_root / f"{config.output_basename}.labels.json"

        if self.force or self._needs_pack(objects, output, link_script):
            self._link_intermediate(
                config, info, objects, intermediate, link_script, triple=triple or DEFAULT_FREESTANDING_TRIPLE
            )
            self._pack_flat(config, info, intermediate, output, labels_path, seg_root)
            progress.update(status=output.name)
        else:
            progress.update(status=f"{output.name} (cached)")

        progress.finish("ok")
        self._log(f"    flat {output} ({compiled} compiled, {skipped} cached)")
        return BuildResult(
            config=config,
            output=output,
            labels_path=labels_path,
            compile_commands=root_db,
            objects=objects,
            compiled=compiled,
            skipped=skipped,
            product_kind=product,
        )

    def _select_triple(self, config: ProjectConfig, product: str) -> str | None:
        if config.target_triple:
            return config.target_triple
        if product == "nxflat":
            return DEFAULT_FREESTANDING_TRIPLE
        if product == "library" and config.library_kind == "next":
            return DEFAULT_FREESTANDING_TRIPLE
        # host library / host executable: native clang target (Windows COFF).
        return None

    def _compile_argv(
        self,
        config: ProjectConfig,
        compiler: str,
        unit: ModuleUnit,
        object_path: Path,
        *,
        pcm_path: Path | None,
        pcm_root: Path,
        pcm_by_module: dict[str, Path],
        triple: str | None,
    ) -> list[str]:
        argv = [compiler]
        if triple:
            argv += ["-target", triple]

        if unit.path.suffix not in ASM_SUFFIXES:
            standard = config.standard or "c++20"
            argv.append(f"-std={standard}")

        for flag in config.compile_flags:
            argv.append(flag)

        # All project-owned BMIs built so far are passed by name below. Do not
        # search pcm_root implicitly: files left by older builds can have the
        # name of a dependency module and silently override its current BMI.
        for module_path in config.module_paths:
            argv.append(f"-fprebuilt-module-path={module_path}")
        for name, path in sorted(pcm_by_module.items()):
            argv.append(f"-fmodule-file={name}={path}")

        for define in config.defines:
            argv.append(f"-D{define}")

        if unit.produces_bmi and pcm_path is not None:
            argv.append(f"-fmodule-output={pcm_path}")

        argv += ["-c", str(unit.path.resolve()), "-o", str(object_path.resolve())]
        return argv

    def _archive_library(
        self,
        config: ProjectConfig,
        info,
        objects: list[Path],
        build_root: Path,
    ) -> Path:
        archiver = find_tool(info, "llvm-ar", "ar")
        if os.name == "nt" and (config.library_kind == "host" or config.target_triple is None):
            output = build_root / f"{config.output_basename}.lib"
        else:
            output = build_root / f"lib{config.output_basename}.a"

        if not self.force and output.exists():
            out_mtime = output.stat().st_mtime
            if objects and all(output.exists() and obj.stat().st_mtime <= out_mtime for obj in objects):
                return output

        if output.exists():
            output.unlink()
        argv = [archiver, "rcs", str(output), *[str(obj) for obj in objects]]
        if not config.draw_progress:
            self._log("    ar " + " ".join(argv[1:]))
        run_command(argv)
        return output

    def _link_host(
        self,
        config: ProjectConfig,
        info,
        objects: list[Path],
        build_root: Path,
        *,
        triple: str | None,
    ) -> Path:
        output = build_root / config.output_basename
        if os.name == "nt" and output.suffix == "":
            output = output.with_suffix(".exe")

        argv = [info.binary]
        if triple:
            argv += ["-target", triple]

        freestanding = any(
            flag in {"-ffreestanding", "-nostdlib"} or flag.startswith("-nostd")
            for flag in config.compile_flags + config.link_flags
        )
        if freestanding or config.link_libraries:
            # Host apps that consume freestanding host-stdlib must not pull MSVC CRT.
            if not any(flag == "-fuse-ld=lld" or flag.startswith("-fuse-ld=") for flag in config.link_flags):
                argv.append("-fuse-ld=lld")
            if not any(flag == "-nostdlib" for flag in config.link_flags):
                argv.append("-nostdlib")
            if os.name == "nt" and not any("subsystem" in flag.lower() for flag in config.link_flags):
                argv.append("-Wl,/subsystem:console")

        for flag in config.link_flags:
            argv.append(flag)
        argv.extend(str(path) for path in objects)
        argv.extend(str(path) for path in config.link_libraries)
        argv.extend(["-o", str(output)])
        if not config.draw_progress:
            self._log("    ld " + " ".join(argv[1:]))
        run_command(argv)
        return output

    def _link_intermediate(
        self,
        config: ProjectConfig,
        info,
        objects: list[Path],
        intermediate: Path,
        link_script: Path,
        *,
        triple: str,
    ) -> None:
        argv = [
            info.binary,
            "-target",
            triple,
            "-nostdlib",
            "-static",
            "-fuse-ld=lld",
            f"-Wl,-T,{link_script}",
            "-Wl,--build-id=none",
            "-Wl,--no-dynamic-linker",
        ]
        for flag in config.link_flags:
            argv.append(flag)
        argv.extend(str(path) for path in objects)
        argv.extend(str(path) for path in config.link_libraries)
        argv.extend(["-o", str(intermediate)])
        if not config.draw_progress:
            self._log("    ld " + " ".join(argv[1:]))
        run_command(argv)

    def _pack_flat(
        self,
        config: ProjectConfig,
        info,
        intermediate: Path,
        output: Path,
        labels_path: Path,
        seg_root: Path,
    ) -> None:
        objcopy = find_tool(info, "llvm-objcopy", "objcopy")
        segments: list[tuple[str, int, bytes]] = []

        for name, address in sorted(config.executable_layout.items(), key=lambda i: i[1]):
            blob_path = seg_root / f"{name}.bin"
            run_command(
                [
                    objcopy,
                    "-O",
                    "binary",
                    "-j",
                    f".{name}",
                    str(intermediate),
                    str(blob_path),
                ]
            )
            blob = blob_path.read_bytes() if blob_path.exists() else b""
            segments.append((name, address, blob))
            self._log(f"    seg {name}: 0x{address:X} +{len(blob)} bytes")

        image = pack_labeled_flat(segments)
        write_nxfl(output, image)
        write_labels_sidecar(labels_path, image, layout=config.executable_layout)

    def _needs_rebuild(self, source: Path, outputs: Iterable[Path]) -> bool:
        outputs = list(outputs)
        if self.force or not outputs or any(not path.exists() for path in outputs):
            return True
        source_mtime = source.stat().st_mtime
        return any(source_mtime > path.stat().st_mtime for path in outputs)

    def _needs_pack(self, objects: Iterable[Path], output: Path, link_script: Path) -> bool:
        if self.force or not output.exists():
            return True
        output_mtime = output.stat().st_mtime
        if link_script.exists() and link_script.stat().st_mtime > output_mtime:
            return True
        return any(path.stat().st_mtime > output_mtime for path in objects)

    def _log(self, message: str) -> None:
        if not self.quiet:
            print(message)


def object_path_for(source: Path, source_root: Path, obj_root: Path) -> Path:
    try:
        relative = source.resolve().relative_to(source_root.resolve())
    except ValueError:
        relative = Path(source.name)
    return obj_root / relative.with_suffix(".o")


def discover_projects(main_root: Path) -> list[Path]:
    projects: list[Path] = []
    for path in sorted(main_root.rglob(".nextbuild")):
        if path.is_file() and path.stat().st_size > 0:
            if "docs" in path.parts:
                continue
            projects.append(path)
    return projects


def resolve_nextbuild_path(target: Path, main_root: Path) -> Path:
    path = target if target.is_absolute() else (main_root / target)
    path = path.resolve()
    if path.is_dir():
        candidate = path / ".nextbuild"
        if candidate.is_file():
            return candidate
        raise BuildError(f"no .nextbuild in directory: {path}")
    if path.name == ".nextbuild" or path.suffix == ".nextbuild" or path.name.endswith(".nextbuild"):
        if not path.is_file():
            raise BuildError(f".nextbuild file not found: {path}")
        return path
    raise BuildError(f"expected project directory or .nextbuild file: {target}")

