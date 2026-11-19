from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def entry_for(*, directory: Path, source: Path, arguments: list[str]) -> dict[str, Any]:
    return {
        "directory": str(directory.resolve()),
        "file": str(source.resolve()),
        "arguments": [str(arg) for arg in arguments],
    }


def write_compile_commands(path: Path, entries: list[dict[str, Any]]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    ordered = sorted(entries, key=lambda item: item["file"])
    path.write_text(json.dumps(ordered, indent=2) + "\n", encoding="utf-8")
    return path


def merge_compile_commands(
    path: Path,
    entries: list[dict[str, Any]],
    *,
    replace_under: Path | None = None,
) -> Path:
    """Merge entries into an existing DB.

    If `replace_under` is set, drop previous entries whose file path is under that
    directory (so rebuilding one project refreshes only its units).
    """
    existing: list[dict[str, Any]] = []
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, list):
                existing = [item for item in loaded if isinstance(item, dict) and "file" in item]
        except (OSError, json.JSONDecodeError):
            existing = []

    keep: list[dict[str, Any]] = []
    replace_root = replace_under.resolve() if replace_under is not None else None
    for item in existing:
        file_path = Path(item["file"])
        if replace_root is not None:
            try:
                file_path.resolve().relative_to(replace_root)
                continue
            except ValueError:
                pass
        keep.append(item)

    by_file = {item["file"]: item for item in keep}
    for item in entries:
        by_file[item["file"]] = item
    return write_compile_commands(path, list(by_file.values()))
