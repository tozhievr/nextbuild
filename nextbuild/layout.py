"""Compatibility re-exports — layout packing lives in flat.py."""

from .flat import generate_section_script as generate_linker_script
from .flat import write_section_script as write_linker_script

__all__ = ["generate_linker_script", "write_linker_script"]
