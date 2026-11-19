from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(slots=True)
class Node:
    line: int = 0
    column: int = 0


@dataclass(slots=True)
class StringLit(Node):
    value: str = ""


@dataclass(slots=True)
class NumberLit(Node):
    raw: str = ""

    def as_int(self) -> int:
        return int(self.raw, 0)

    def as_version(self) -> tuple[int, ...]:
        return tuple(int(part) for part in self.raw.split("."))


@dataclass(slots=True)
class BoolLit(Node):
    value: bool = False


@dataclass(slots=True)
class Ident(Node):
    name: str = ""


@dataclass(slots=True)
class VariableRef(Node):
    name: str = ""


@dataclass(slots=True)
class PathExpr(Node):
    parts: list[Node] = field(default_factory=list)


@dataclass(slots=True)
class BlockItem(Node):
    key: str = ""
    value: Node | None = None


@dataclass(slots=True)
class BlockExpr(Node):
    items: list[BlockItem] = field(default_factory=list)


Expr = StringLit | NumberLit | BoolLit | Ident | VariableRef | PathExpr | BlockExpr


@dataclass(slots=True)
class Assignment(Node):
    """`Name : expr` or `@VAR : expr`."""

    target: str = ""
    is_variable: bool = False
    value: Expr | None = None


@dataclass(slots=True)
class Document:
    path: str | None
    statements: list[Assignment] = field(default_factory=list)


def expr_preview(node: Node | None) -> str:
    if node is None:
        return "<empty>"
    if isinstance(node, StringLit):
        return repr(node.value)
    if isinstance(node, NumberLit):
        return node.raw
    if isinstance(node, BoolLit):
        return str(node.value)
    if isinstance(node, Ident):
        return node.name
    if isinstance(node, VariableRef):
        return f"@{node.name}"
    if isinstance(node, PathExpr):
        return " / ".join(expr_preview(part) for part in node.parts)
    if isinstance(node, BlockExpr):
        inner = ", ".join(f"{item.key}={expr_preview(item.value)}" for item in node.items)
        return "{" + inner + "}"
    return type(node).__name__


def literal_value(node: Node) -> Any:
    if isinstance(node, StringLit):
        return node.value
    if isinstance(node, NumberLit):
        if "." in node.raw and not node.raw.lower().startswith("0x"):
            return node.raw
        try:
            return node.as_int()
        except ValueError:
            return node.raw
    if isinstance(node, BoolLit):
        return node.value
    if isinstance(node, Ident):
        return node.name
    raise TypeError(f"not a literal: {type(node).__name__}")
