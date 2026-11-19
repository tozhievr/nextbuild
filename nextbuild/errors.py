from __future__ import annotations


class NextBuildError(Exception):
    """Base error for the nextbuild system."""


class ParseError(NextBuildError):
    def __init__(self, message: str, *, path: str | None = None, line: int = 0, column: int = 0):
        self.path = path
        self.line = line
        self.column = column
        location = ""
        if path:
            location = f"{path}:{line}:{column}: "
        elif line:
            location = f"line {line}, column {column}: "
        super().__init__(f"{location}{message}")


class EvalError(NextBuildError):
    """Raised when a .nextbuild expression cannot be evaluated."""


class BuildError(NextBuildError):
    """Raised when compilation, linking, or toolchain checks fail."""
