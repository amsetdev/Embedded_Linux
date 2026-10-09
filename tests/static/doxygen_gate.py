#!/usr/bin/env python3
"""Doxygen gate: every function, struct, member, macro and global of the built
C code must be documented. Fails on ANY doxygen warning.

Also checks that the Doxyfile documents exactly what the makefile builds: every
SRCS file is inside INPUT and not excluded, and every unbuilt src/*.c is in
EXCLUDE (an unbuilt file would otherwise fail the gate for code nobody ships).

Usage:
    python3 tests/static/doxygen_gate.py       # HTML in build-docs/html/index.html

Output: build-docs/html/ and build-docs/doxygen-warnings.log.
"""

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from srcs import REPO, built_sources, unbuilt_sources  # noqa: E402

DOXYFILE = REPO / "Doxyfile"
WARN_LOG = REPO / "build-docs" / "doxygen-warnings.log"


def doxyfile_list(name):
    text = DOXYFILE.read_text().replace("\\\n", " ")
    m = re.search(rf"^{name}\s*=\s*(.*)$", text, re.M)
    return m.group(1).split() if m else []


def check_doxyfile():
    problems = []
    inputs, excluded = doxyfile_list("INPUT"), set(doxyfile_list("EXCLUDE"))
    for src in built_sources():
        if src in excluded:
            problems.append(f"{src} is built (makefile SRCS) but EXCLUDEd in the Doxyfile")
        elif not any(src == i or src.startswith(i.rstrip("/") + "/") for i in inputs):
            problems.append(f"{src} is built (makefile SRCS) but not under the Doxyfile INPUT")
    for src in unbuilt_sources():
        if src not in excluded:
            problems.append(f"{src} is not built: add it (and its header) to EXCLUDE in the Doxyfile")
    return problems


def main():
    problems = check_doxyfile()
    if problems:
        print("Doxyfile does not match the makefile:\n  " + "\n  ".join(problems))
        return 1

    WARN_LOG.parent.mkdir(exist_ok=True)
    proc = subprocess.run(["doxygen", str(DOXYFILE)], cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-2000:], proc.stderr[-2000:])
        return proc.returncode
    warnings = [l for l in WARN_LOG.read_text().splitlines() if l.strip()]
    warnings = [l.replace(str(REPO) + "/", "") for l in warnings]
    if warnings:
        print(f"doxygen: {sum('warning:' in l for l in warnings)} warning(s), document these "
              f"(style: DOCS/TESTS_GUIDE.md §4.1):")
        for line in warnings:
            print(f"  {line}")
        return 1
    print(f"doxygen: 0 warnings, HTML in {(REPO / 'build-docs/html/index.html').relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
