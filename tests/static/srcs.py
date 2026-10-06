"""The C files the firmware actually builds: SRCS in the makefile.

Shared by the static gates so they always check exactly what is compiled.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def built_sources():
    """SRCS from the makefile, as repo-relative paths (e.g. 'src/main.c')."""
    text = (REPO / "makefile").read_text().replace("\\\n", " ")
    m = re.search(r"^SRCS\s*:=\s*(.*)$", text, re.M)
    if not m:
        raise RuntimeError("SRCS not found in the makefile")
    return [s.replace("$(SRC_DIR)", "src") for s in m.group(1).split()]


def unbuilt_sources():
    """C files under src/ that the makefile does not build."""
    built = set(built_sources())
    return sorted(str(p.relative_to(REPO)) for p in (REPO / "src").rglob("*.c") if str(p.relative_to(REPO)) not in built)
