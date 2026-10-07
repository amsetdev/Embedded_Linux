#!/usr/bin/env python3
"""Write <build dir>/build_info.json: what was built, from which commit, with what.

Run after `make all` (CI job build-firmware, DOCS/CI_CD_GUIDE.md §5.1):
    python3 tools/ci/build_info.py build

The version is APP_VERSION_DEF from inc/settings.h (the release tag must be
v<that version>). Every runtime library collected into build/lib/ is listed
with its SHA256, so a deployed board can be compared against a pipeline.
Uses only the standard library (the build image has a bare python3).
"""

import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SETTINGS_H = REPO / "inc" / "settings.h"
COMPILER = "arm-linux-gnueabihf-gcc"


def app_version():
    """APP_VERSION_DEF from inc/settings.h, or exit if it can't be found."""
    m = re.search(r'#define\s+APP_VERSION_DEF\s+"([^"]+)"', SETTINGS_H.read_text())
    if not m:
        sys.exit(f"APP_VERSION_DEF not found in {SETTINGS_H.relative_to(REPO)}")
    return m.group(1)


def sha256(path):
    """Hex SHA256 of a file."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*cmd):
    """First line of a command's output, or "" if it fails."""
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return ""
    return out.strip().splitlines()[0] if out.strip() else ""


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    binary = build / "main"
    if not binary.is_file():
        sys.exit(f"{binary} not found: run `make all` first")

    libs = sorted((build / "lib").glob("*.so*")) if (build / "lib").is_dir() else []
    env = os.environ.get
    info = {
        "app_version": app_version(),
        "git_sha": env("CI_COMMIT_SHA") or run("git", "rev-parse", "HEAD"),
        "git_ref": env("CI_COMMIT_REF_NAME") or run("git", "rev-parse", "--abbrev-ref", "HEAD"),
        "pipeline_id": env("CI_PIPELINE_ID", "local"),
        "build_timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "target": "STM32MP157F-DK2 Cortex-A7, arm-linux-gnueabihf",
        "compiler": run(COMPILER, "--version"),
        "binary": binary.name,
        "binary_file_type": run("file", "-b", str(binary)),
        "binary_size_bytes": binary.stat().st_size,
        "binary_sha256": sha256(binary),
        "libs": {lib.name: sha256(lib) for lib in libs},
    }
    out = build / "build_info.json"
    out.write_text(json.dumps(info, indent=2) + "\n")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
