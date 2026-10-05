"""What the firmware actually reads from a config file.

Builds tests/validate/fw_config_dump.c together with the firmware's own
src/util/settings.c, src/util/json.c and src/fieldbus/data.c for the host
(gcc with AddressSanitizer + UndefinedBehaviorSanitizer) and runs it on a
config. The result is the firmware's view: every AppSettings field and every
parsed register, exactly as the board would see them.

    from firmware_view import firmware_view, firmware_defaults
    view = firmware_view(config_text)      # dict, see fw_config_dump.c
    view["fields"]["wifi.enable"]["value"]

Needs a C compiler (`cc`, override with $CC). The binary is cached in
build-host/validate/ and rebuilt when any source changes.
"""

import json
import os
import shutil
import subprocess
import tempfile
from functools import lru_cache
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "tests" / "validate" / "fw_config_dump.c"
FIRMWARE_SOURCES = [
    REPO / "src" / "util" / "settings.c",
    REPO / "src" / "util" / "json.c",
    REPO / "src" / "fieldbus" / "data.c",
]
BUILD_DIR = REPO / "build-host" / "validate"
BINARY = BUILD_DIR / "fw_config_dump"
CONFIG_NAME = "smart_rtu_config.json"   # SETTINGS_FILE in inc/settings.h

CFLAGS = ["-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
          "-fno-omit-frame-pointer"]

# The firmware's strncpy-style code is fine under ASan; leak checking needs
# ptrace, which some container setups forbid.
SAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0",
           "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"}


class ParserCrash(Exception):
    """The firmware parser crashed or a sanitizer reported an error on this input."""


def _sources():
    return [HARNESS, *FIRMWARE_SOURCES, *sorted((REPO / "inc").glob("*.h"))]


@lru_cache(maxsize=1)
def build():
    """Compile the harness if needed; return the binary path."""
    if BINARY.exists() and all(BINARY.stat().st_mtime >= s.stat().st_mtime for s in _sources()):
        return BINARY
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise RuntimeError("no C compiler found (install gcc, or set $CC); "
                           "the validate stage runs the real firmware parser")
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [cc, *CFLAGS, "-I", str(REPO / "inc"), "-o", str(BINARY),
           str(HARNESS), *map(str, FIRMWARE_SOURCES)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"building the firmware parser harness failed:\n{' '.join(cmd)}\n{proc.stderr}")
    return BINARY


def _run(args):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "view.json"
        proc = subprocess.run([str(build()), str(out), *args], capture_output=True, text=True,
                              env={**os.environ, **SAN_ENV}, timeout=60)
        if proc.returncode != 0 or not out.exists():
            raise ParserCrash(f"firmware parser exited with {proc.returncode}:\n{proc.stderr[-3000:]}")
        return json.loads(out.read_text())


def firmware_view(config_text):
    """Run settings_load() + parse_registers() on config_text (str or bytes)."""
    data = config_text.encode() if isinstance(config_text, str) else config_text
    with tempfile.TemporaryDirectory() as cfg_dir:
        (Path(cfg_dir) / CONFIG_NAME).write_bytes(data)
        return _run([cfg_dir])


@lru_cache(maxsize=1)
def firmware_defaults():
    """AppSettings after settings_defaults(): what a missing key falls back to."""
    return _run([])
