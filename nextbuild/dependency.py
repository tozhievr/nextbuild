from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .ast import (
    Assignment,
    BlockExpr,
    BoolLit,
    Ident,
    NumberLit,
    PathExpr,
    StringLit,
    VariableRef,
)
from .errors import EvalError, ParseError
from .parser import Parser


@dataclass
class DependencySpec:
    path: Path
    name: str = "Dependency"
    version: str = "0.0.0"
    library_kind: str | None = None
    require_project: Path | None = None
    # Unevaluated AST nodes — resolved after provider build with @DEP_* vars.
    module_path_nodes: list[Any] = field(default_factory=list)
    link_library_nodes: list[Any] = field(default_factory=list)
    compile_flags: list[str] = field(default_factory=list)
    link_flags: list[str] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    use_dependencies: list[Path] = field(default_factory=list)


_DEP_DIRECTIVES = {
    "Dependency",
    "Version",
    "LibraryKind",
    "RequireProject",
    "AddModulePath",
    "LinkLibrary",
    "EnableCompileFlag",
    "EnableLinkFlag",
    "AddDefine",
    "UseDependency",
}


def eval_expr(node: Any, variables: dict[str, Any]) -> Any:
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
        if node.name not in variables:
            raise EvalError(f"undefined variable @{node.name}")
        return variables[node.name]
    if isinstance(node, PathExpr):
        parts = [eval_expr(part, variables) for part in node.parts]
        result = Path(str(parts[0]))
        for part in parts[1:]:
            result = result / str(part)
        return result
    if isinstance(node, BlockExpr):
        raise EvalError("blocks are not supported in dependency expressions")
    raise EvalError(f"cannot evaluate {type(node).__name__}")


class DependencyLoader:
    def __init__(self, document, *, main_root: Path, builtins: dict[str, Any] | None = None):
        self.document = document
        self.variables: dict[str, Any] = {
            "NEXTOS_MAIN_ROOT": main_root.resolve(),
            **(builtins or {}),
        }

    def load(self) -> DependencySpec:
        path = Path(self.document.path) if self.document.path else Path("<memory>")
        spec = DependencySpec(path=path)

        for statement in self.document.statements:
            assert statement.value is not None
            if statement.is_variable:
                self.variables[statement.target] = eval_expr(statement.value, self.variables)
                continue
            self._apply(spec, statement)
        return spec

    def _apply(self, spec: DependencySpec, statement: Assignment) -> None:
        name = statement.target
        value = statement.value
        assert value is not None
        if name not in _DEP_DIRECTIVES:
            raise ParseError(
                f"unknown dependency directive '{name}'",
                path=self.document.path,
                line=statement.line,
                column=statement.column,
            )
        if name == "Dependency":
            spec.name = str(eval_expr(value, self.variables))
        elif name == "Version":
            spec.version = str(eval_expr(value, self.variables))
        elif name == "LibraryKind":
            spec.library_kind = str(eval_expr(value, self.variables)).lower()
        elif name == "RequireProject":
            path = eval_expr(value, self.variables)
            spec.require_project = Path(path) if not isinstance(path, Path) else path
        elif name == "AddModulePath":
            spec.module_path_nodes.append(value)
        elif name == "LinkLibrary":
            spec.link_library_nodes.append(value)
        elif name == "EnableCompileFlag":
            spec.compile_flags.append(str(eval_expr(value, self.variables)))
        elif name == "EnableLinkFlag":
            spec.link_flags.append(str(eval_expr(value, self.variables)))
        elif name == "AddDefine":
            spec.defines.append(str(eval_expr(value, self.variables)))
        elif name == "UseDependency":
            path = eval_expr(value, self.variables)
            spec.use_dependencies.append(path if isinstance(path, Path) else Path(str(path)))


def load_dependency(
    path: str | Path,
    *,
    main_root: Path,
    builtins: dict[str, Any] | None = None,
) -> DependencySpec:
    document = Parser.parse_file(path)
    return DependencyLoader(document, main_root=main_root, builtins=builtins).load()


def resolve_dependency_exports(
    spec: DependencySpec,
    *,
    main_root: Path,
    build_root: Path,
    library_path: Path | None,
) -> tuple[list[Path], list[Path]]:
    """Resolve AddModulePath / LinkLibrary against provider outputs."""
    variables: dict[str, Any] = {
        "NEXTOS_MAIN_ROOT": main_root.resolve(),
        "DEP_BUILD_ROOT": build_root.resolve(),
        "DEP_PCM": (build_root / "pcm").resolve(),
        "DEP_LIBRARY": (library_path or (build_root / "library")).resolve(),
    }
    modules: list[Path] = []
    for node in spec.module_path_nodes:
        value = eval_expr(node, variables)
        modules.append(value if isinstance(value, Path) else Path(value))

    libraries: list[Path] = []
    for node in spec.link_library_nodes:
        value = eval_expr(node, variables)
        libraries.append(value if isinstance(value, Path) else Path(value))
    return modules, libraries
