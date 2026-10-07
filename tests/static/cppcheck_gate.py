#!/usr/bin/env python3
"""Run cppcheck on the built C sources and fail only on findings NOT in the baseline.

Checked files: SRCS in the makefile (tests/static/srcs.py), so exactly what is
compiled; src/fieldbus/modbus.c and src/control_logic.c are not built and not
checked. Existing findings are recorded in cppcheck-baseline.json; a commit that
adds a new one fails CI, one that fixes old ones just shrinks the list (re-run
with --update-baseline to lock that in). Findings are matched by file + check
id + message (not line number), so editing code above an old finding doesn't
make it "new". Each identical finding is counted, so a second copy is still new.

Usage:
    python3 tests/static/cppcheck_gate.py                    # check (CI)
    python3 tests/static/cppcheck_gate.py --update-baseline  # accept current findings

Also writes reports/cppcheck.xml and reports/gl-code-quality.json (GitLab Code
Quality format, shown in merge requests).
"""

import argparse
import hashlib
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from srcs import REPO, built_sources  # noqa: E402

BASELINE = Path(__file__).with_name("cppcheck-baseline.json")
XML_OUT = REPO / "reports" / "cppcheck.xml"
CODEQUALITY_OUT = REPO / "reports" / "gl-code-quality.json"

CPPCHECK_ARGS = [
    "cppcheck",
    "--enable=warning,style,performance,portability",
    "--check-level=exhaustive",
    "--inline-suppr",
    "--suppress=missingIncludeSystem",   # armhf library headers aren't on the host
    "--suppress=missingInclude",
    "--suppress=unmatchedSuppression",
    "--suppress=checkersReport",
    "-D__GNUC__",
    "-D__linux__",
    "--std=c11",
    "--platform=unix32",                 # Cortex-A7: 32-bit, ILP32
    "-I", "inc",
    "-j4",
    "--xml", "--xml-version=2",
]

SEVERITY_TO_GITLAB = {
    "error": "critical", "warning": "major", "portability": "minor",
    "performance": "minor", "style": "info", "information": "info",
}


def run_cppcheck():
    XML_OUT.parent.mkdir(exist_ok=True)
    proc = subprocess.run(CPPCHECK_ARGS + built_sources(), cwd=REPO, capture_output=True, text=True)
    XML_OUT.write_text(proc.stderr)  # cppcheck writes its XML report to stderr
    if proc.returncode != 0 or "<results" not in proc.stderr:
        sys.exit(f"cppcheck failed to run (exit {proc.returncode}):\n{proc.stderr[-2000:]}")
    return parse(proc.stderr)


def parse(xml_text):
    findings = []
    for err in ET.fromstring(xml_text).iter("error"):
        loc = err.find("location")
        if loc is None:
            continue  # whole-program notes without a source location
        findings.append({
            "file": loc.get("file"),
            "line": int(loc.get("line", 0)),
            "severity": err.get("severity"),
            "id": err.get("id"),
            "message": err.get("msg"),
        })
    return sorted(findings, key=lambda f: (f["file"], f["line"], f["id"]))


def key(f):
    return f"{f['file']}|{f['id']}|{f['message']}"


def write_codequality(findings):
    report = []
    for f in findings:
        report.append({
            "description": f"{f['id']}: {f['message']}",
            "check_name": f["id"],
            "fingerprint": hashlib.md5(f"{key(f)}|{f['line']}".encode()).hexdigest(),
            "severity": SEVERITY_TO_GITLAB.get(f["severity"], "info"),
            "location": {"path": f["file"], "lines": {"begin": f["line"]}},
        })
    CODEQUALITY_OUT.write_text(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--update-baseline", action="store_true", help="record current findings as accepted")
    args = parser.parse_args()

    findings = run_cppcheck()
    write_codequality(findings)
    current = Counter(key(f) for f in findings)

    if args.update_baseline:
        BASELINE.write_text(json.dumps(dict(sorted(current.items())), indent=2) + "\n")
        print(f"Baseline updated: {sum(current.values())} findings in {BASELINE.relative_to(REPO)}")
        return 0

    baseline = Counter(json.loads(BASELINE.read_text())) if BASELINE.exists() else Counter()
    new = current - baseline
    fixed = baseline - current

    print(f"cppcheck: {sum(current.values())} findings, baseline {sum(baseline.values())}, "
          f"new {sum(new.values())}, fixed {sum(fixed.values())}")
    if fixed:
        print("\nFixed since baseline (run with --update-baseline to lock in):")
        for k in sorted(fixed):
            print(f"  - {k.replace('|', ': ', 2)}")
    if new:
        print("\nNEW findings (fix them, or add '// cppcheck-suppress <id>' with a reason):")
        shown = Counter()
        for f in findings:
            k = key(f)
            if shown[k] < new[k]:
                shown[k] += 1
                print(f"  {f['file']}:{f['line']}: {f['severity']}: {f['id']}: {f['message']}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
