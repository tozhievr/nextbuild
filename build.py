#!/usr/bin/env python3
"""NextOS Build System — entry point for nextbuild."""

from __future__ import annotations

import sys
from pathlib import Path

# Allow `python tools/build.py` without installing the package.
_TOOLS_DIR = Path(__file__).resolve().parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from nextbuild.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
