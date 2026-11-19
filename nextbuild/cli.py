from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import __version__
from .builder import Builder, discover_projects
from .errors import NextBuildError


def find_main_root(start: Path | None = None) -> Path:
    """Locate the NextOS repository root (directory that contains tools/build.py)."""
    here = (start or Path.cwd()).resolve()
    for candidate in (here, *here.parents):
        marker = candidate / "tools" / "build.py"
        if marker.is_file():
            return candidate
    # Fallback: package lives in <root>/tools/nextbuild
    return Path(__file__).resolve().parents[2]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="nextbuild",
        description="NextOS nextbuild — build freestanding projects from .nextbuild files",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="project dirs or .nextbuild files (default: all non-empty .nextbuild under repo root)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="NextOS main root (defines @NEXTOS_MAIN_ROOT)",
    )
    parser.add_argument("--clean", action="store_true", help="remove build roots instead of building")
    parser.add_argument("--force", "-B", action="store_true", help="rebuild even if outputs are fresh")
    parser.add_argument("--list", action="store_true", help="list discovered .nextbuild projects and exit")
    parser.add_argument("--quiet", "-q", action="store_true", help="suppress non-error output")
    parser.add_argument("--version", action="version", version=f"nextbuild {__version__}")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    main_root = (args.root or find_main_root()).resolve()

    try:
        if args.list:
            projects = discover_projects(main_root)
            if not projects:
                print(f"no projects under {main_root}")
                return 0
            for path in projects:
                print(path.relative_to(main_root) if path.is_relative_to(main_root) else path)
            return 0

        builder = Builder(
            main_root=main_root,
            force=args.force,
            quiet=args.quiet,
        )

        targets = [Path(item) for item in args.targets]
        if args.clean:
            if not targets:
                targets = discover_projects(main_root)
            for target in targets:
                builder.clean_path(target)
            return 0

        if targets:
            results = [builder.build_path(target) for target in targets]
        else:
            results = builder.build_all()

        built = sum(1 for result in results if result.output is not None)
        if not args.quiet:
            print(f"nextbuild: finished {built}/{len(results)} project(s)")
        return 0
    except NextBuildError as exc:
        print(f"nextbuild: error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("nextbuild: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
