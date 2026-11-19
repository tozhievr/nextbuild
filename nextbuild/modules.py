from __future__ import annotations

import re
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path

from .errors import BuildError

MODULE_SUFFIXES = {".cppm", ".ixx", ".ccm", ".cxxm", ".mxx"}
IMPL_SUFFIXES = {".cpp", ".cc", ".cxx", ".c++"}
ASM_SUFFIXES = {".s", ".S", ".asm"}
SOURCE_SUFFIXES = MODULE_SUFFIXES | IMPL_SUFFIXES | ASM_SUFFIXES

_EXPORT_MODULE_RE = re.compile(
    r"^\s*export\s+module\s+([\w.]+)(?:\s*:\s*([\w.]+))?\s*;",
    re.MULTILINE,
)
_MODULE_RE = re.compile(
    r"^\s*module\s+([\w.]+)(?:\s*:\s*([\w.]+))?\s*;",
    re.MULTILINE,
)
_IMPORT_RE = re.compile(
    r"^\s*(?:export\s+)?import\s+(?:([\w.]+)|:([\w.]+))\s*;",
    re.MULTILINE,
)
_HEADER_IMPORT_RE = re.compile(
    r"^\s*(?:export\s+)?import\s+(?:<[^>]+>|\"[^\"]+\")\s*;",
    re.MULTILINE,
)
_INCLUDE_RE = re.compile(r"^\s*#\s*include\b", re.MULTILINE)


class UnitKind(Enum):
    INTERFACE = auto()  # export module X;
    PARTITION_INTERFACE = auto()  # export module X:part;
    IMPLEMENTATION = auto()  # module X;
    PARTITION_IMPL = auto()  # module X:part;
    ASSEMBLY = auto()
    TRANSLATION = auto()


@dataclass(slots=True)
class ModuleUnit:
    path: Path
    kind: UnitKind
    module: str | None = None
    partition: str | None = None
    imports: list[str] = field(default_factory=list)

    @property
    def logical_name(self) -> str:
        if self.module and self.partition:
            return f"{self.module}:{self.partition}"
        if self.module:
            return self.module
        return self.path.stem

    @property
    def produces_bmi(self) -> bool:
        return self.kind in {UnitKind.INTERFACE, UnitKind.PARTITION_INTERFACE}

    @property
    def primary_module(self) -> str | None:
        return self.module


def scan_unit(path: Path) -> ModuleUnit:
    text = path.read_text(encoding="utf-8-sig")
    suffix = path.suffix

    if suffix in ASM_SUFFIXES:
        return ModuleUnit(path=path, kind=UnitKind.ASSEMBLY)

    if _HEADER_IMPORT_RE.search(text) or _INCLUDE_RE.search(text):
        raise BuildError(
            f"{path}: header imports/#include are not allowed — NextOS is modules-only"
        )

    imports: list[str] = []
    for match in _IMPORT_RE.finditer(text):
        if match.group(1):
            imports.append(match.group(1))
        elif match.group(2):
            imports.append(f":{match.group(2)}")

    export_match = _EXPORT_MODULE_RE.search(text)
    if export_match:
        module, partition = export_match.group(1), export_match.group(2)
        kind = UnitKind.PARTITION_INTERFACE if partition else UnitKind.INTERFACE
        return ModuleUnit(
            path=path,
            kind=kind,
            module=module,
            partition=partition,
            imports=imports,
        )

    module_match = _MODULE_RE.search(text)
    if module_match:
        module, partition = module_match.group(1), module_match.group(2)
        kind = UnitKind.PARTITION_IMPL if partition else UnitKind.IMPLEMENTATION
        return ModuleUnit(
            path=path,
            kind=kind,
            module=module,
            partition=partition,
            imports=imports,
        )

    if suffix in MODULE_SUFFIXES:
        raise BuildError(f"{path}: module interface file without 'export module' declaration")

    return ModuleUnit(path=path, kind=UnitKind.TRANSLATION, imports=imports)


def collect_units(source_root: Path) -> list[ModuleUnit]:
    if not source_root.exists():
        raise BuildError(f"source directory does not exist: {source_root}")
    if source_root.is_file():
        if source_root.suffix not in SOURCE_SUFFIXES:
            return []
        return [scan_unit(source_root)]

    units: list[ModuleUnit] = []
    for path in sorted(source_root.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            units.append(scan_unit(path))
    return units


def order_units(units: list[ModuleUnit]) -> list[ModuleUnit]:
    """Interfaces first (by import topo), then partitions, implementations, other, asm."""
    interfaces = {
        unit.module: unit
        for unit in units
        if unit.kind is UnitKind.INTERFACE and unit.module
    }
    partitions = [unit for unit in units if unit.kind is UnitKind.PARTITION_INTERFACE]
    implementations = [
        unit
        for unit in units
        if unit.kind in {UnitKind.IMPLEMENTATION, UnitKind.PARTITION_IMPL}
    ]
    assembly = [unit for unit in units if unit.kind is UnitKind.ASSEMBLY]
    other = [unit for unit in units if unit.kind is UnitKind.TRANSLATION]

    graph: dict[str, set[str]] = {name: set() for name in interfaces}
    for name, unit in interfaces.items():
        for dep in unit.imports:
            if not dep.startswith(":") and dep in interfaces and dep != name:
                graph[name].add(dep)

    ordered_names: list[str] = []
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(name: str) -> None:
        if name in visited:
            return
        if name in visiting:
            raise BuildError(f"cyclic module import involving '{name}'")
        visiting.add(name)
        for dep in sorted(graph.get(name, ())):
            visit(dep)
        visiting.remove(name)
        visited.add(name)
        ordered_names.append(name)

    for name in sorted(graph):
        visit(name)

    ordered: list[ModuleUnit] = [interfaces[name] for name in ordered_names]
    ordered.extend(sorted(partitions, key=lambda u: u.logical_name))

    def impl_key(unit: ModuleUnit) -> tuple[int, str]:
        primary = unit.module or ""
        depth = ordered_names.index(primary) if primary in ordered_names else len(ordered_names)
        return (depth, unit.path.as_posix())

    ordered.extend(sorted(implementations, key=impl_key))
    ordered.extend(sorted(other, key=lambda u: u.path.as_posix()))
    ordered.extend(sorted(assembly, key=lambda u: u.path.as_posix()))
    return ordered


def pcm_filename(unit: ModuleUnit) -> str:
    return unit.logical_name.replace(":", "-") + ".pcm"
