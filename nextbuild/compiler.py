from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .config import parse_version
from .errors import BuildError


_VERSION_RE = re.compile(
    r"(?:clang|gcc|g\+\+|Apple LLVM)\s+version\s+(\d+(?:\.\d+)*)",
    re.IGNORECASE,
)


@dataclass(frozen=True, slots=True)
class CompilerInfo:
    binary: str
    version: tuple[int, ...]
    version_text: str
    bindir: Path


def resolve_compiler(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise BuildError(f"compiler not found on PATH: {name}")
    return path


def probe_compiler(compiler: str) -> CompilerInfo:
    binary = resolve_compiler(compiler)
    try:
        completed = subprocess.run(
            [binary, "--version"],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise BuildError(f"failed to execute compiler '{binary}': {exc}") from exc

    output = (completed.stdout or "") + "\n" + (completed.stderr or "")
    match = _VERSION_RE.search(output)
    if not match:
        first_line = output.splitlines()[0] if output.splitlines() else ""
        dotted = re.search(r"\d+(?:\.\d+)+", first_line)
        if not dotted:
            raise BuildError(f"unable to parse compiler version from:\n{output.strip()}")
        version_text = dotted.group(0)
    else:
        version_text = match.group(1)

    return CompilerInfo(
        binary=binary,
        version=parse_version(version_text),
        version_text=version_text,
        bindir=Path(binary).resolve().parent,
    )


def ensure_min_version(info: CompilerInfo, minimum: tuple[int, ...] | None) -> None:
    if minimum is None:
        return
    if info.version < minimum:
        need = ".".join(str(part) for part in minimum)
        have = ".".join(str(part) for part in info.version)
        raise BuildError(
            f"compiler {info.binary} version {have} is older than required {need}"
        )


def find_tool(info: CompilerInfo, *names: str) -> str:
    for name in names:
        candidate = info.bindir / name
        if candidate.is_file():
            return str(candidate)
        with_exe = info.bindir / f"{name}.exe"
        if with_exe.is_file():
            return str(with_exe)
        found = shutil.which(name)
        if found:
            return found
    raise BuildError(f"required tool not found next to clang or on PATH: {', '.join(names)}")


def run_command(argv: list[str], *, cwd: str | None = None) -> None:
    try:
        completed = subprocess.run(
            argv,
            check=False,
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise BuildError(f"failed to run {' '.join(argv)}: {exc}") from exc

    if completed.returncode != 0:
        details = (completed.stderr or completed.stdout or "").strip()
        joined = " ".join(argv)
        raise BuildError(f"command failed ({completed.returncode}): {joined}\n{details}")
