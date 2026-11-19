from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .ast import (
    Assignment,
    BlockExpr,
    BoolLit,
    Document,
    Ident,
    NumberLit,
    PathExpr,
    StringLit,
    VariableRef,
    expr_preview,
)
from .catalog import DIRECTIVES as DIRECTIVE_CATALOG
from .errors import EvalError, ParseError
from .parser import Parser


def parse_version(text: str) -> tuple[int, ...]:
    parts: list[int] = []
    for piece in text.split("."):
        if not piece.isdigit():
            raise EvalError(f"invalid version number: {text!r}")
        parts.append(int(piece))
    return tuple(parts)


@dataclass
class ProjectConfig:
    path: Path
    project: str = "Untitled"
    version: str = "0.0.0"
    build_root: Path | None = None
    source: Path | None = None
    standard: str | None = None
    compiler: str = "clang++"
    min_compiler_ver: tuple[int, ...] | None = None
    draw_progress: bool = False
    compile_flags: list[str] = field(default_factory=list)
    link_flags: list[str] = field(default_factory=list)
    module_paths: list[Path] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    output_name: str | None = None
    target_triple: str | None = None
    executable_layout: dict[str, int] = field(default_factory=dict)
    product_kind: str | None = None  # nxflat | library | host
    library_kind: str | None = None  # host | next
    features: set[str] = field(default_factory=set)
    variables: dict[str, Any] = field(default_factory=dict)
    depends: list[Path] = field(default_factory=list)
    use_dependencies: list[Path] = field(default_factory=list)
    link_libraries: list[Path] = field(default_factory=list)

    @property
    def output_basename(self) -> str:
        return self.output_name or self.project.lower()

    def resolved_product_kind(self) -> str:
        if self.product_kind:
            return self.product_kind.lower()
        if self.executable_layout:
            return "nxflat"
        if self.library_kind:
            return "library"
        raise EvalError(
            f"{self.path}: set SetProductKind (nxflat|library|host) "
            "or SetExecutableLayout / SetLibraryKind"
        )


class ConfigBuilder:
    """Evaluate a parsed .nextbuild document into ProjectConfig."""

    DIRECTIVES = set(DIRECTIVE_CATALOG)

    def __init__(self, document: Document, *, main_root: Path, builtins: dict[str, Any] | None = None):
        self.document = document
        self.main_root = main_root.resolve()
        self.variables: dict[str, Any] = {
            "NEXTOS_MAIN_ROOT": self.main_root,
            **(builtins or {}),
        }

    def build(self) -> ProjectConfig:
        path = Path(self.document.path) if self.document.path else Path("<memory>")
        config = ProjectConfig(path=path, variables=self.variables)

        for statement in self.document.statements:
            assert statement.value is not None
            if statement.is_variable:
                self.variables[statement.target] = self._eval(statement.value)
                config.variables = dict(self.variables)
                continue
            self._apply_directive(config, statement)

        if config.build_root is None:
            config.build_root = self.main_root / ".build" / config.output_basename
        if config.source is None:
            config.source = path.parent
        return config

    def _apply_directive(self, config: ProjectConfig, statement: Assignment) -> None:
        name = statement.target
        value = statement.value
        assert value is not None

        if name not in self.DIRECTIVES:
            raise ParseError(
                f"unknown directive '{name}'",
                path=self.document.path,
                line=statement.line,
                column=statement.column,
            )

        if name == "Project":
            config.project = self._as_string(value, name)
        elif name == "Version":
            config.version = self._as_string(value, name)
        elif name == "SetBuildRoot":
            config.build_root = self._as_path(value, name)
        elif name == "SetSource":
            config.source = self._as_path(value, name)
        elif name == "SetStandard":
            config.standard = self._as_string(value, name)
        elif name == "SetCompiler":
            config.compiler = self._as_string(value, name)
        elif name == "SetMinCompilerVer":
            config.min_compiler_ver = parse_version(self._as_string(value, name))
        elif name == "SetNextBuildDrawProgress":
            config.draw_progress = self._as_bool(value, name)
        elif name == "EnableCompileFlag":
            config.compile_flags.append(self._as_string(value, name))
        elif name == "EnableLinkFlag":
            config.link_flags.append(self._as_string(value, name))
        elif name == "AddModulePath":
            config.module_paths.append(self._as_path(value, name))
        elif name == "AddDefine":
            config.defines.append(self._as_string(value, name))
        elif name == "SetOutputName":
            config.output_name = self._as_string(value, name)
        elif name == "SetTargetTriple":
            config.target_triple = self._as_string(value, name)
        elif name == "SetExecutableLayout":
            config.executable_layout = self._as_layout(value, name)
        elif name == "SetProductKind":
            kind = self._as_string(value, name).lower()
            if kind not in {"nxflat", "library", "host"}:
                raise EvalError(f"SetProductKind must be nxflat|library|host, got {kind!r}")
            config.product_kind = kind
        elif name == "SetLibraryKind":
            kind = self._as_string(value, name).lower()
            if kind not in {"host", "next"}:
                raise EvalError(f"SetLibraryKind must be host|next, got {kind!r}")
            config.library_kind = kind
        elif name == "EnableFeature":
            config.features.add(self._as_string(value, name).upper())
        elif name == "UseDependency":
            config.use_dependencies.append(self._as_path(value, name))
        elif name == "LinkLibrary":
            config.link_libraries.append(self._as_path(value, name))
        elif name == "Depend":
            config.depends.append(self._as_path(value, name))

    def _eval(self, node: Any) -> Any:
        if isinstance(node, StringLit):
            return node.value
        if isinstance(node, NumberLit):
            if node.raw.lower().startswith("0x"):
                return node.as_int()
            if "." in node.raw:
                return node.raw
            return node.as_int()
        if isinstance(node, BoolLit):
            return node.value
        if isinstance(node, Ident):
            return node.name
        if isinstance(node, VariableRef):
            if node.name not in self.variables:
                raise EvalError(f"undefined variable @{node.name}")
            return self.variables[node.name]
        if isinstance(node, PathExpr):
            parts = [self._eval(part) for part in node.parts]
            return self._join_path_parts(parts)
        if isinstance(node, BlockExpr):
            return {
                item.key: self._eval(item.value)
                for item in node.items
                if item.value is not None
            }
        raise EvalError(f"cannot evaluate {type(node).__name__}")

    def _join_path_parts(self, parts: list[Any]) -> Path:
        if not parts:
            raise EvalError("empty path expression")
        result = Path(str(parts[0]))
        for part in parts[1:]:
            text = str(part)
            # Absolute override only if later part is absolute on its own.
            piece = Path(text)
            result = result / piece
        return result

    def _as_string(self, node: Any, directive: str) -> str:
        value = self._eval(node)
        if isinstance(value, Path):
            return str(value)
        if isinstance(value, bool):
            return "True" if value else "False"
        return str(value)

    def _as_bool(self, node: Any, directive: str) -> bool:
        value = self._eval(node)
        if isinstance(value, bool):
            return value
        raise EvalError(f"{directive} expects a boolean, got {expr_preview(node)}")

    def _as_path(self, node: Any, directive: str) -> Path:
        value = self._eval(node)
        path = Path(value) if not isinstance(value, Path) else value
        return path

    def _as_layout(self, node: Any, directive: str) -> dict[str, int]:
        value = self._eval(node)
        if not isinstance(value, dict):
            raise EvalError(f"{directive} expects a block, got {expr_preview(node)}")
        layout: dict[str, int] = {}
        for key, raw in value.items():
            if isinstance(raw, int):
                layout[key] = raw
            elif isinstance(raw, str):
                layout[key] = int(raw, 0)
            else:
                raise EvalError(f"{directive}: address for '{key}' must be an integer")
        return layout


def load_project(path: str | Path, *, main_root: Path, builtins: dict[str, Any] | None = None) -> ProjectConfig:
    document = Parser.parse_file(path)
    return ConfigBuilder(document, main_root=main_root, builtins=builtins).build()
