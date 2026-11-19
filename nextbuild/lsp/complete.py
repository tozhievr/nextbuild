from __future__ import annotations

import re
from pathlib import Path

from nextbuild.catalog import (
    BOOL_DIRECTIVES,
    BUILTIN_VARIABLES,
    COMMON_COMPILE_FLAGS,
    COMPILER_SUGGESTIONS,
    DEPENDENCY_PATH_DIRECTIVES,
    DEP_BUILTIN_VARIABLES,
    FEATURE_SUGGESTIONS,
    LAYOUT_DIRECTIVES,
    LIBRARY_KIND_SUGGESTIONS,
    PATH_DIRECTIVES,
    PRODUCT_KIND_SUGGESTIONS,
    STANDARD_SUGGESTIONS,
    directive_markdown,
)

from .analyze import DocAnalysis, allowed_directives, known_variables, line_text

_KIND_KEYWORD = 14
_KIND_VARIABLE = 6
_KIND_FILE = 17
_KIND_FOLDER = 19
_KIND_VALUE = 12
_KIND_SNIPPET = 15
_KIND_CONSTANT = 21


def completions(
    analysis: DocAnalysis,
    *,
    line: int,
    character: int,
    main_root: Path,
) -> list[dict]:
    current = line_text(analysis.text, line)
    prefix = current[:character]
    stripped = prefix.lstrip()
    directives = allowed_directives(analysis)
    path_dirs = DEPENDENCY_PATH_DIRECTIVES if analysis.is_dependency else PATH_DIRECTIVES

    if (
        not analysis.is_dependency
        and _in_layout_block(analysis.text, line)
        and re.match(r"^\s*-?\s*[A-Za-z_]*$", prefix)
    ):
        return _layout_item_completions(prefix)

    at = prefix.rfind("@")
    if at >= 0 and re.match(r"^@[A-Za-z0-9_]*$", prefix[at:]):
        return _variable_completions(analysis, prefix[at + 1 :], after_at=True)

    path_typed = _path_context(prefix, path_dirs)
    if path_typed is not None:
        return _path_completions(path_typed, main_root, analysis)

    if ":" in prefix:
        left, right = prefix.split(":", 1)
        directive = left.strip()
        if directive.startswith("@"):
            return _generic_value_completions(right.lstrip(), analysis)
        if directive in directives:
            return _value_completions(
                directive, right.lstrip(), analysis, main_root, path_dirs
            )

    if re.match(r"^[@A-Za-z_]\w*$", stripped) or stripped in {"", "@"}:
        items = _directive_completions(stripped, analysis)
        if stripped.startswith("@") or stripped == "":
            items.extend(
                _variable_completions(
                    analysis,
                    stripped[1:],
                    after_at=stripped.startswith("@"),
                )
            )
        return items

    return []


def hover(analysis: DocAnalysis, *, line: int, character: int) -> dict | None:
    word = _word_at(analysis.text, line, character)
    if not word:
        return None
    if word.startswith("@"):
        name = word[1:]
        docs = known_variables(analysis)
        if name in docs:
            return {
                "contents": {
                    "kind": "markdown",
                    "value": f"**@{name}**\n\n{docs[name]}",
                }
            }
        return None
    info = allowed_directives(analysis).get(word)
    if info:
        return {"contents": {"kind": "markdown", "value": directive_markdown(info)}}
    return None


def definition(
    analysis: DocAnalysis,
    *,
    line: int,
    character: int,
    uri: str,
) -> dict | None:
    word = _word_at(analysis.text, line, character)
    if not word or not word.startswith("@"):
        return None
    name = word[1:]
    builtins = DEP_BUILTIN_VARIABLES if analysis.is_dependency else BUILTIN_VARIABLES
    if name in builtins:
        return None
    defined = analysis.variables.get(name)
    if not defined:
        return None
    return {
        "uri": uri,
        "range": {
            "start": {"line": defined.line, "character": defined.character},
            "end": {
                "line": defined.line,
                "character": defined.character + 1 + len(name),
            },
        },
    }


def _directive_completions(typed: str, analysis: DocAnalysis) -> list[dict]:
    items: list[dict] = []
    needle = typed.lstrip("@")
    if typed.startswith("@"):
        return items
    for name, info in allowed_directives(analysis).items():
        if needle and not name.lower().startswith(needle.lower()):
            continue
        if info.value_kind == "layout":
            insert, kind, fmt = (
                name + " : {\n    - ${1:start} = ${2:0x10000}\n}",
                _KIND_SNIPPET,
                2,
            )
        elif info.value_kind == "bool":
            insert, kind, fmt = (name + " : ${1|True,False|}", _KIND_SNIPPET, 2)
        elif name == "UseDependency":
            insert, kind, fmt = (
                "UseDependency : @NEXTOS_MAIN_ROOT / '${1:stdlib}' / '${2:host}' / '.nextbuild-dependency'",
                _KIND_SNIPPET,
                2,
            )
        elif name == "SetProductKind":
            insert, kind, fmt = (
                "SetProductKind : ${1|nxflat,library,host|}",
                _KIND_SNIPPET,
                2,
            )
        elif name in {"SetLibraryKind", "LibraryKind"}:
            insert, kind, fmt = (
                f"{name} : ${{1|host,next|}}",
                _KIND_SNIPPET,
                2,
            )
        elif info.value_kind == "path":
            if analysis.is_dependency and name in {"AddModulePath", "LinkLibrary"}:
                var = "@DEP_PCM" if name == "AddModulePath" else "@DEP_LIBRARY"
                insert, kind, fmt = (f"{name} : {var}", _KIND_SNIPPET, 2)
            else:
                insert, kind, fmt = (
                    name + " : @NEXTOS_MAIN_ROOT / '${1:path}'",
                    _KIND_SNIPPET,
                    2,
                )
        else:
            insert, kind, fmt = (name + " : ", _KIND_KEYWORD, 1)
        items.append(
            {
                "label": name,
                "kind": kind,
                "detail": info.summary,
                "documentation": {"kind": "markdown", "value": directive_markdown(info)},
                "insertText": insert,
                "insertTextFormat": fmt,
            }
        )
    if typed == "":
        items.append(
            {
                "label": "@VARIABLE",
                "kind": _KIND_VARIABLE,
                "detail": "Define a nextbuild variable",
                "insertText": "@${1:NAME} : ${2:value}",
                "insertTextFormat": 2,
            }
        )
        if analysis.is_dependency:
            items.insert(
                0,
                {
                    "label": "dependency skeleton",
                    "kind": _KIND_SNIPPET,
                    "detail": "Scaffold a .nextbuild-dependency export",
                    "insertText": "\n".join(
                        [
                            "Dependency : ${1:LibName}",
                            "Version : ${2:0.1.0}",
                            "LibraryKind : ${3|host,next|}",
                            "",
                            "RequireProject : @NEXTOS_MAIN_ROOT / '${4:stdlib}' / '${5:host}'",
                            "",
                            "AddModulePath : @DEP_PCM",
                            "LinkLibrary : @DEP_LIBRARY",
                            "",
                            "EnableCompileFlag : '-nostdinc++'",
                            "",
                        ]
                    ),
                    "insertTextFormat": 2,
                },
            )
    return items


def _variable_completions(
    analysis: DocAnalysis,
    typed: str,
    *,
    after_at: bool,
) -> list[dict]:
    items: list[dict] = []
    for name, doc in known_variables(analysis).items():
        if typed and not name.lower().startswith(typed.lower()):
            continue
        items.append(
            {
                "label": f"@{name}",
                "kind": _KIND_VARIABLE,
                "detail": doc,
                "insertText": name if after_at else f"@{name}",
                "filterText": f"@{name}",
            }
        )
    return items


def _value_completions(
    directive: str,
    typed: str,
    analysis: DocAnalysis,
    main_root: Path,
    path_dirs: set[str],
) -> list[dict]:
    if directive in BOOL_DIRECTIVES:
        return [
            {"label": "True", "kind": _KIND_CONSTANT, "insertText": "True"},
            {"label": "False", "kind": _KIND_CONSTANT, "insertText": "False"},
        ]
    if directive == "SetStandard":
        needle = typed.strip().strip("'\"")
        return [
            {"label": value, "kind": _KIND_VALUE, "insertText": value}
            for value in STANDARD_SUGGESTIONS
            if not needle or value.startswith(needle)
        ]
    if directive == "SetCompiler":
        return [
            {"label": value, "kind": _KIND_VALUE, "insertText": value}
            for value in COMPILER_SUGGESTIONS
            if not typed or value.startswith(typed)
        ]
    if directive == "SetProductKind":
        return [
            {"label": value, "kind": _KIND_VALUE, "insertText": value}
            for value in PRODUCT_KIND_SUGGESTIONS
            if not typed or value.startswith(typed)
        ]
    if directive in {"SetLibraryKind", "LibraryKind"}:
        return [
            {"label": value, "kind": _KIND_VALUE, "insertText": value}
            for value in LIBRARY_KIND_SUGGESTIONS
            if not typed or value.startswith(typed)
        ]
    if directive == "EnableFeature":
        needle = typed.strip().upper()
        return [
            {"label": value, "kind": _KIND_VALUE, "insertText": value}
            for value in FEATURE_SUGGESTIONS
            if not needle or value.startswith(needle)
        ]
    if directive == "EnableCompileFlag":
        return [
            {"label": flag, "kind": _KIND_VALUE, "insertText": flag}
            for flag in COMMON_COMPILE_FLAGS
            if not typed or typed.strip("'") in flag or flag.startswith(typed)
        ]
    if directive in path_dirs:
        items = _path_completions(typed, main_root, analysis)
        if analysis.is_dependency and typed == "":
            if directive == "AddModulePath":
                items.insert(
                    0,
                    {
                        "label": "@DEP_PCM",
                        "kind": _KIND_VARIABLE,
                        "insertText": "@DEP_PCM",
                        "detail": DEP_BUILTIN_VARIABLES["DEP_PCM"],
                    },
                )
            if directive == "LinkLibrary":
                items.insert(
                    0,
                    {
                        "label": "@DEP_LIBRARY",
                        "kind": _KIND_VARIABLE,
                        "insertText": "@DEP_LIBRARY",
                        "detail": DEP_BUILTIN_VARIABLES["DEP_LIBRARY"],
                    },
                )
        return items
    if directive in LAYOUT_DIRECTIVES and typed.strip() in {"", "{"}:
        return [
            {
                "label": "{ layout }",
                "kind": _KIND_SNIPPET,
                "insertText": "{\n    - ${1:start} = ${2:0x10000}\n}",
                "insertTextFormat": 2,
            }
        ]
    return _generic_value_completions(typed, analysis)


def _generic_value_completions(typed: str, analysis: DocAnalysis) -> list[dict]:
    items: list[dict] = []
    if typed.startswith("@"):
        items.extend(_variable_completions(analysis, typed[1:], after_at=True))
    elif typed == "":
        items.extend(_variable_completions(analysis, "", after_at=False))
        root_snippet = (
            "@DEP_BUILD_ROOT / '${1:path}'"
            if analysis.is_dependency
            else "@NEXTOS_MAIN_ROOT / '${1:path}'"
        )
        items.append(
            {
                "label": root_snippet.replace("${1:path}", "…"),
                "kind": _KIND_SNIPPET,
                "insertText": root_snippet,
                "insertTextFormat": 2,
            }
        )
        items.extend(
            [
                {"label": "True", "kind": _KIND_CONSTANT, "insertText": "True"},
                {"label": "False", "kind": _KIND_CONSTANT, "insertText": "False"},
            ]
        )
    return items


def _path_context(prefix: str, path_dirs: set[str]) -> str | None:
    if ":" not in prefix:
        return None
    left, right = prefix.split(":", 1)
    directive = left.strip()
    if directive in path_dirs or directive.startswith("@"):
        return right.lstrip()
    if "/" in right:
        return right.lstrip()
    return None


def _path_completions(typed: str, main_root: Path, analysis: DocAnalysis) -> list[dict]:
    items: list[dict] = []
    if typed == "" or (typed.startswith("@") and " " not in typed and "/" not in typed):
        items.extend(
            _variable_completions(
                analysis,
                typed[1:] if typed.startswith("@") else "",
                after_at=typed.startswith("@"),
            )
        )
        if typed == "":
            items.append(
                {
                    "label": "@NEXTOS_MAIN_ROOT / '…'",
                    "kind": _KIND_SNIPPET,
                    "insertText": "@NEXTOS_MAIN_ROOT / '${1:path}'",
                    "insertTextFormat": 2,
                }
            )

    quote_match = re.search(r"(?:^|/\s*)'([^']*)$", typed)
    if not quote_match:
        quote_match = re.search(r'(?:^|/\s*)"([^"]*)$', typed)
    if not quote_match:
        if typed.rstrip().endswith("/") or re.search(r"/\s*$", typed):
            partial = ""
            completed_segments = _quoted_segments(typed)
        else:
            return items
    else:
        partial = quote_match.group(1)
        completed_segments = _quoted_segments(typed[: quote_match.start(1)])

    base = main_root
    for segment in completed_segments:
        base = base / segment

    try:
        if not base.is_dir():
            return items
        for entry in sorted(base.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower())):
            if partial and not entry.name.lower().startswith(partial.lower()):
                continue
            try:
                rel = str(entry.relative_to(main_root))
            except ValueError:
                rel = str(entry)
            items.append(
                {
                    "label": entry.name + ("/" if entry.is_dir() else ""),
                    "kind": _KIND_FOLDER if entry.is_dir() else _KIND_FILE,
                    "detail": rel,
                    "insertText": entry.name,
                }
            )
    except OSError:
        pass
    return items


def _quoted_segments(text: str) -> list[str]:
    return re.findall(r"['\"]([^'\"]+)['\"]", text)


def _in_layout_block(text: str, line: int) -> bool:
    lines = text.splitlines()[: line + 1]
    depth = 0
    seen_layout = False
    for row in lines:
        if "SetExecutableLayout" in row:
            seen_layout = True
        depth += row.count("{") - row.count("}")
    return seen_layout and depth > 0


def _layout_item_completions(prefix: str) -> list[dict]:
    has_dash = bool(re.search(r"^\s*-", prefix))
    names = ("start", "main", "text", "rodata", "data", "bss", "entry")
    items = []
    for name in names:
        insert = f"{name} = 0x${{1:10000}}" if has_dash else f"- {name} = 0x${{1:10000}}"
        items.append(
            {
                "label": name,
                "kind": _KIND_SNIPPET,
                "insertText": insert,
                "insertTextFormat": 2,
                "detail": f"Layout segment label → section .{name}",
            }
        )
    return items


def _word_at(text: str, line: int, character: int) -> str | None:
    current = line_text(text, line)
    if character > len(current):
        character = len(current)
    start = character
    while start > 0 and (current[start - 1].isalnum() or current[start - 1] in "_@"):
        start -= 1
    end = character
    while end < len(current) and (current[end].isalnum() or current[end] in "_"):
        end += 1
    word = current[start:end]
    return word or None
