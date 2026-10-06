#!/usr/bin/env python3
"""Run the host C test binaries and write a JUnit report from Unity output.

Usage: python tests/host/run_tests.py <build_dir> [junit.xml]

Unity prints one line per test: file:line:test_name:PASS|FAIL|IGNORE[: msg].
IGNORE (KNOWN_ISSUE) becomes <skipped>. A binary that crashes (e.g. an
AddressSanitizer report) or exits non-zero without a FAIL line is reported as
an <error> with the tail of its output. Exit code is non-zero on any failure.
"""

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

LINE = re.compile(r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s?(?P<msg>.*))?$", re.MULTILINE)


def run(binary):
    proc = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    output = proc.stdout + proc.stderr
    print(output, end="")
    return proc.returncode, output


def main(build_dir, junit_path):
    binaries = sorted(p for p in build_dir.glob("test_*") if p.is_file() and p.stat().st_mode & 0o111)
    if not binaries:
        sys.exit(f"no test_* binaries in {build_dir}")

    root = ET.Element("testsuites")
    failed = False
    for binary in binaries:
        suite = ET.SubElement(root, "testsuite", name=binary.name)
        code, output = run(binary)
        counts = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
        for m in LINE.finditer(output.replace("\r", "")):
            if not m.group(0).strip():
                continue
            case = ET.SubElement(suite, "testcase", classname=binary.name, name=m["name"],
                                 file=m["file"], line=m["line"])
            counts["tests"] += 1
            if m["status"] == "FAIL":
                ET.SubElement(case, "failure", message=m["msg"] or "").text = m.group(0)
                counts["failures"] += 1
            elif m["status"] == "IGNORE":
                ET.SubElement(case, "skipped", message=m["msg"] or "")
                counts["skipped"] += 1

        if code != 0 and counts["failures"] == 0:
            case = ET.SubElement(suite, "testcase", classname=binary.name, name="process")
            ET.SubElement(case, "error", message=f"exit code {code} (crash / sanitizer?)").text = output[-4000:]
            counts["tests"] += 1
            counts["errors"] += 1

        for k, v in counts.items():
            suite.set(k, str(v))
        failed |= code != 0 or counts["failures"] > 0 or counts["errors"] > 0
        print(f"--- {binary.name}: {counts}\n")

    junit_path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(junit_path, encoding="utf-8", xml_declaration=True)
    return 1 if failed else 0


if __name__ == "__main__":
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build-host")
    report = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("reports/host-tests.xml")
    sys.exit(main(build, report))
