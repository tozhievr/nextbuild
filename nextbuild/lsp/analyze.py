from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote, urlparse
from urllib.request import url2pathname

from nextbuild.catalog import (
    BUILTIN_VARIABLES,
    DEPENDENCY_DIRECTIVES,
    DEP_BUILTIN_VARIABLES,
    DIRECTIVES,
    is_dependency_document,
)
from nextbuild.errors import ParseError
from nextbuild.lexer import Lexer, Token
from nextbuild.parser import Parser


@dataclass
class VarDef:
    name: str
    line: int  # 0-based
    character: int


@dataclass
class DocAnalysis:
    path: str | None
    text: str
    is_dependency: bool = False
    tokens: list[Token] = field(default_factory=list)
    diagnostics: list[dict] = field(default_factory=list)
    variables: dict[str, VarDef] = field(default_factory=dict)
    parse_ok: bool = True


def uri_to_path(uri: str) -> Path:
    parsed = urlparse(uri)
    if parsed.scheme != "file":
        return Path(uri)
    raw = unquote(parsed.path)
    converted = url2pathname(raw)
    return Path(converted)


def path_to_uri(path: Path) -> str:
    resolved = path.resolve().as_posix()
    if re.match(r"^[A-Za-z]:/", resolved):
        return "file:///" + resolved
    return "file://" + resolved


def find_main_root(start: Path | None = None) -> Path:
    here = (start or Path.cwd()).resolve()
    for candidate in (here, *here.parents):
        if (candidate / "tools" / "build.py").is_file():
            return candidate
    return Path(__file__).resolve().parents[3]


def analyze_document(text: str, *, path: str | None = None) -> DocAnalysis:
    analysis = DocAnalysis(
        path=path,
        text=text,
        is_dependency=is_dependency_document(path),
    )
    allowed = DEPENDENCY_DIRECTIVES if analysis.is_dependency else DIRECTIVES

    try:
        analysis.tokens = Lexer(text, path=path).tokenize()
    except ParseError as exc:
        analysis.parse_ok = False
        analysis.diagnostics.append(_diag(exc.line, exc.column, str(exc), severity=1))
        return analysis

    try:
        document = Parser(analysis.tokens, path=path).parse()
    except ParseError as exc:
        analysis.parse_ok = False
        analysis.diagnostics.append(_diag(exc.line, exc.column, str(exc), severity=1))
        return analysis

    for statement in document.statements:
        if statement.is_variable:
            analysis.variables[statement.target] = VarDef(
                name=statement.target,
                line=max(statement.line - 1, 0),
                character=max(statement.column - 1, 0),
            )
            continue
        if statement.target not in allowed:
            kind = "dependency directive" if analysis.is_dependency else "directive"
            analysis.diagnostics.append(
                _diag(
                    statement.line,
                    statement.column,
                    f"unknown {kind} '{statement.target}'",
                    severity=1,
                    end_character=statement.column - 1 + len(statement.target),
                )
            )
    return analysis


def _diag(
    line: int,
    column: int,
    message: str,
    *,
    severity: int,
    end_character: int | None = None,
) -> dict:
    start_line = max(line - 1, 0)
    start_col = max(column - 1, 0)
    end_col = end_character if end_character is not None else start_col + 1
    clean = re.sub(r"^.*?:\d+:\d+:\s*", "", message)
    clean = re.sub(r"^line \d+, column \d+:\s*", "", clean)
    return {
        "range": {
            "start": {"line": start_line, "character": start_col},
            "end": {"line": start_line, "character": max(end_col, start_col + 1)},
        },
        "severity": severity,
        "source": "nextbuild",
        "message": clean,
    }


def line_text(text: str, line: int) -> str:
    lines = text.splitlines()
    if line < 0 or line >= len(lines):
        return ""
    return lines[line]


def known_variables(analysis: DocAnalysis) -> dict[str, str]:
    result = dict(DEP_BUILTIN_VARIABLES if analysis.is_dependency else BUILTIN_VARIABLES)
    for name in analysis.variables:
        suffix = (
            "`.nextbuild-dependency` file."
            if analysis.is_dependency
            else "`.nextbuild` file."
        )
        result[name] = f"User variable defined in this {suffix}"
    return result


def allowed_directives(analysis: DocAnalysis) -> dict:
    return DEPENDENCY_DIRECTIVES if analysis.is_dependency else DIRECTIVES
