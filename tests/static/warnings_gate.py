#!/usr/bin/env python3
"""Fail the build on compiler warnings that are NOT in the baseline.

The makefile compiles with -Wall -Wextra. The warnings that existed when this
gate was added are recorded in compiler-warnings-baseline.json; a change that
adds a new one fails CI, one that fixes old ones just shrinks the list (re-run
with --update-baseline to lock that in). Warnings are matched by
file + function + warning flag + message (not line number), so editing code
above an old warning doesn't make it "new". Identical warnings are counted, so
a second copy is still new.

Warnings that GCC reports inside a system header (e.g. -Wstringop-truncation in
glibc's string_fortified.h) are attributed to the project line they were
inlined from.

Usage (the log must come from a CLEAN build, `make clean all`):
    python3 tests/static/warnings_gate.py build/build.log                    # check (CI)
    python3 tests/static/warnings_gate.py build/build.log --update-baseline  # accept current warnings

Also writes reports/gl-code-quality-warnings.json (GitLab Code Quality format).
"""

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).with_name("compiler-warnings-baseline.json")
CODEQUALITY_OUT = REPO / "reports" / "gl-code-quality-warnings.json"

COMPILER = "arm-linux-gnueabihf-gcc"
PROJECT_FILE = r"(?:\./)?(?:src|inc)/[^:\s]+"

RE_COMPILE = re.compile(rf"^{re.escape(COMPILER)}\s")
RE_IN_FUNCTION = re.compile(rf"^(?P<file>{PROJECT_FILE}): In function '(?P<func>[^']+)':")
RE_TOP_LEVEL = re.compile(rf"^(?P<file>{PROJECT_FILE}): At top level:")
RE_INLINED = re.compile(rf"inlined from '(?P<func>[^']+)' at (?P<file>{PROJECT_FILE}):(?P<line>\d+)")
RE_WARNING = re.compile(
    r"^(?P<file>[^:\s]+):(?P<line>\d+):(?:\d+:)? warning: (?P<msg>.*?)(?: \[(?P<flag>-W[^\]]+)\])?$")
RE_LINK_WARNING = re.compile(r"^\S*ld(?:\.\w+)?: warning: (?P<msg>.*)$")


def normalise(text):
    """Make messages locale-independent: GCC uses ‘’ quotes in UTF-8 locales."""
    return text.replace("‘", "'").replace("’", "'")


def parse(log_text):
    """Return a list of warnings {file, line, func, flag, message} from a make log."""
    warnings = []
    func = ""          # function of the last "In function" header in a project file
    inlined = None     # (file, line, func) of the last "inlined from ... at src/..."
    for raw in log_text.splitlines():
        line = normalise(raw.rstrip())
        if RE_COMPILE.match(line):
            func, inlined = "", None
            continue
        m = RE_IN_FUNCTION.match(line)
        if m:
            func, inlined = m["func"], None
            continue
        if RE_TOP_LEVEL.match(line):
            func, inlined = "", None
            continue
        m = RE_INLINED.search(line)
        if m:
            inlined = (m["file"], int(m["line"]), m["func"])
            continue
        m = RE_WARNING.match(line)
        if m:
            file, lineno, where = m["file"], int(m["line"]), func
            if not re.fullmatch(PROJECT_FILE, file) and inlined:
                file, lineno, where = inlined
            warnings.append({
                "file": file.removeprefix("./"),
                "line": lineno,
                "func": where,
                "flag": m["flag"] or "",
                "message": m["msg"],
            })
            inlined = None
            continue
        m = RE_LINK_WARNING.match(line)
        if m:
            warnings.append({"file": "<link>", "line": 0, "func": "", "flag": "", "message": m["msg"]})
    return sorted(warnings, key=lambda w: (w["file"], w["line"], w["flag"]))


def key(w):
    return f"{w['file']}|{w['func']}|{w['flag']}|{w['message']}"


def write_codequality(warnings):
    report = []
    for w in warnings:
        report.append({
            "description": f"{w['flag'] or 'warning'}: {w['message']}",
            "check_name": w["flag"] or "compiler-warning",
            "fingerprint": hashlib.md5(f"{key(w)}|{w['line']}".encode()).hexdigest(),
            "severity": "major",
            "location": {"path": w["file"], "lines": {"begin": w["line"]}},
        })
    CODEQUALITY_OUT.parent.mkdir(exist_ok=True)
    CODEQUALITY_OUT.write_text(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path, help="output of a clean `make all` (stdout + stderr)")
    parser.add_argument("--update-baseline", action="store_true", help="record current warnings as accepted")
    args = parser.parse_args()

    log_text = args.log.read_text(errors="replace")
    if not any(RE_COMPILE.match(l) for l in log_text.splitlines()):
        sys.exit(f"{args.log}: no '{COMPILER}' compile lines - not a clean build log "
                 "(run `make clean all 2>&1 | tee build/build.log`)")

    warnings = parse(log_text)
    write_codequality(warnings)
    current = Counter(key(w) for w in warnings)

    if args.update_baseline:
        BASELINE.write_text(json.dumps(dict(sorted(current.items())), indent=2) + "\n")
        print(f"Baseline updated: {sum(current.values())} warnings in {BASELINE.relative_to(REPO)}")
        return 0

    baseline = Counter(json.loads(BASELINE.read_text())) if BASELINE.exists() else Counter()
    new = current - baseline
    fixed = baseline - current

    print(f"compiler warnings: {sum(current.values())}, baseline {sum(baseline.values())}, "
          f"new {sum(new.values())}, fixed {sum(fixed.values())}")
    if fixed:
        print("\nFixed since baseline (run with --update-baseline to lock in):")
        for k in sorted(fixed):
            print(f"  - {k.replace('|', ': ', 3)}")
    if new:
        print("\nNEW warnings (fix them; see DOCS/CI_CD_GUIDE.md 'compiler-warnings'):")
        for w in warnings:
            if new[key(w)] > 0:
                print(f"  {w['file']}:{w['line']}: in {w['func'] or 'file scope'}: warning: {w['message']} [{w['flag']}]")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
