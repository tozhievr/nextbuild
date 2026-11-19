"""NextOS nextbuild — declarative freestanding build system."""

__version__ = "0.1.0"

from .builder import BuildResult, Builder
from .config import ProjectConfig
from .errors import NextBuildError

__all__ = [
    "BuildResult",
    "Builder",
    "NextBuildError",
    "ProjectConfig",
    "__version__",
]
