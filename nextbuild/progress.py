from __future__ import annotations

import sys
from dataclasses import dataclass


@dataclass
class ProgressBar:
    total: int
    enabled: bool = True
    width: int = 28
    label: str = "nextbuild"
    _current: int = 0

    def update(self, step: int = 1, *, status: str = "") -> None:
        self._current = min(self.total, self._current + step)
        if not self.enabled or self.total <= 0:
            return
        ratio = self._current / self.total
        filled = int(self.width * ratio)
        bar = "#" * filled + "-" * (self.width - filled)
        percent = int(ratio * 100)
        suffix = f" {status}" if status else ""
        line = f"\r[{self.label}] [{bar}] {percent:3d}% ({self._current}/{self.total}){suffix}"
        sys.stdout.write(line.ljust(100))
        sys.stdout.flush()
        if self._current >= self.total:
            sys.stdout.write("\n")
            sys.stdout.flush()

    def finish(self, status: str = "done") -> None:
        if self._current < self.total:
            self._current = self.total
            self.update(0, status=status)
        elif self.enabled and self.total == 0:
            print(f"[{self.label}] {status}")
