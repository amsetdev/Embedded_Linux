"""tools/release/make_release.py — tag/version gate, packaging, OTA command safety, publishing.

Packaging runs on a fake build directory; publishing records the HTTP calls
instead of sending them.
"""

import hashlib
import importlib.util
import json
import re
import tarfile
from pathlib import Path

import pytest

pytestmark = pytest.mark.unit

REPO = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location("make_release", REPO / "tools" / "release" / "make_release.py")
mr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mr)

ELF = "ELF 32-bit LSB pie executable, ARM, EABI5 version 1 (SYSV), dynamically linked"


def test_version_read_from_settings_h():
    text = (REPO / "inc" / "settings.h").read_text()
    assert f'APP_VERSION_DEF        "{mr.app_version()}"' in text
    assert re.fullmatch(r"\d+\.\d+\.\d+", mr.app_version())


def test_matching_tag_passes(monkeypatch):
    monkeypatch.setenv("CI_COMMIT_TAG", f"v{mr.app_version()}")
    mr.check_tag(mr.app_version())


@pytest.mark.parametrize("tag", ["v9.9.9", "1.0.0", "v1.0.0-rc1", "release"])
def test_mismatched_tag_fails(monkeypatch, tag):
    monkeypatch.setenv("CI_COMMIT_TAG", tag)
    with pytest.raises(SystemExit) as exc:
        mr.check_tag("1.0.0")
    assert "does not match APP_VERSION_DEF" in str(exc.value)


def test_no_tag_is_allowed(monkeypatch):
    monkeypatch.delenv("CI_COMMIT_TAG", raising=False)
    mr.check_tag("1.0.0")


def test_placeholder_url_is_refused_by_device():
    """An unedited ota_command.json must not start a download: the firmware's
    ota_parse_request() only accepts https:// URLs (src/cloud/ota_logic.c)."""
    url = mr.OTA_URL_PLACEHOLDER.format(file="stm32mp1-gateway_1.0.0")
    assert not url.lower().startswith("https://")
    assert "stm32mp1-gateway_1.0.0" in url
    assert 'strncasecmp(req->url, "https://", 8)' in (REPO / "src/cloud/ota_logic.c").read_text()


def make_package(build, version="1.2.3", skip=()):
    """build/gateway-<version>.tar.gz like `make package` (minus the members in skip)."""
    pkg = build / "package"
    (pkg / "bin").mkdir(parents=True, exist_ok=True)
    (pkg / "lib").mkdir(exist_ok=True)
    (pkg / "bin" / "gateway").write_bytes((build / "main").read_bytes())
    (pkg / "lib" / "libmodbus.so.5").write_bytes(b"modbus")
    (pkg / "lib" / "libmosquitto.so.1").write_bytes(b"mosquitto")
    (pkg / "install.sh").write_text("#!/bin/sh\n")
    (pkg / "gateway.service").write_text("[Service]\n")
    (pkg / "VERSION").write_text(version + "\n")
    with tarfile.open(build / f"gateway-{version}.tar.gz", "w:gz") as tar:
        for f in sorted(pkg.rglob("*")):
            rel = f.relative_to(pkg).as_posix()
            if f.is_file() and rel not in skip and not any(rel.startswith(x + "/") for x in skip):
                tar.add(f, arcname=f"./{rel}")


@pytest.fixture
def build(tmp_path):
    """A fake build-firmware artifact directory for version 1.2.3."""
    b = tmp_path / "build"
    (b / "lib").mkdir(parents=True)
    (b / "main").write_bytes(b"\x7fELF fake arm binary")
    (b / "lib" / "libmodbus.so.5").write_bytes(b"modbus")
    (b / "lib" / "libmosquitto.so.1").write_bytes(b"mosquitto")
    make_package(b)
    info = {"app_version": "1.2.3", "git_sha": "abc123", "pipeline_id": "42",
            "compiler": "arm-linux-gnueabihf-gcc 11.4.0", "binary_file_type": ELF,
            "binary_sha256": hashlib.sha256(b"\x7fELF fake arm binary").hexdigest()}
    (b / "build_info.json").write_text(json.dumps(info))
    return b


def test_package_contents(build, tmp_path, monkeypatch):
    monkeypatch.delenv("OTA_URL", raising=False)
    out = tmp_path / "release"
    files = mr.package(build, out, "1.2.3")
    assert sorted(f.name for f in files) == sorted([
        "stm32mp1-gateway_1.2.3", "gateway-1.2.3.tar.gz", "build_info.json",
        "SHA256SUMS", "ota_command.json", "release_notes.md"])
    assert (out / "stm32mp1-gateway_1.2.3").read_bytes() == (build / "main").read_bytes()
    assert (out / "stm32mp1-gateway_1.2.3").stat().st_mode & 0o111
    assert (out / "gateway-1.2.3.tar.gz").read_bytes() == (build / "gateway-1.2.3.tar.gz").read_bytes()


def test_sha256sums_verify(build, tmp_path):
    out = tmp_path / "release"
    mr.package(build, out, "1.2.3")
    lines = (out / "SHA256SUMS").read_text().splitlines()
    assert len(lines) == 3
    for line in lines:
        digest, name = line.split("  ")
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest


def test_ota_command_matches_binary(build, tmp_path, monkeypatch):
    monkeypatch.delenv("OTA_URL", raising=False)
    out = tmp_path / "release"
    mr.package(build, out, "1.2.3")
    ota = json.loads((out / "ota_command.json").read_text())
    assert ota["version"] == "1.2.3"
    assert ota["sha256"] == hashlib.sha256((out / "stm32mp1-gateway_1.2.3").read_bytes()).hexdigest()
    assert re.fullmatch(r"[0-9a-f]{64}", ota["sha256"])
    assert not ota["url"].startswith("https://")


def test_ota_url_from_ci_variable(build, tmp_path, monkeypatch):
    monkeypatch.setenv("OTA_URL", "https://s3.example/gw/stm32mp1-gateway_1.2.3")
    mr.package(build, tmp_path / "r", "1.2.3")
    assert json.loads((tmp_path / "r" / "ota_command.json").read_text())["url"] == \
        "https://s3.example/gw/stm32mp1-gateway_1.2.3"


def test_release_notes(build, tmp_path):
    out = tmp_path / "release"
    mr.package(build, out, "1.2.3")
    notes = (out / "release_notes.md").read_text()
    sha = hashlib.sha256((out / "stm32mp1-gateway_1.2.3").read_bytes()).hexdigest()
    assert "## STM32MP1 gateway 1.2.3" in notes and sha in notes and "abc123" in notes
    assert "devices/<device_id>/ota/app" in notes and "libmodbus.so.5" in notes
    assert "sh /tmp/gw/install.sh" in notes and "--rollback" in notes


@pytest.mark.parametrize("change, message", [
    (lambda b: (b / "main").unlink(), "missing build artifact"),
    (lambda b: (b / "gateway-1.2.3.tar.gz").unlink(), "missing build artifact"),
    (lambda b: make_package(b, skip=("install.sh",)), "missing: install.sh"),
    (lambda b: make_package(b, skip=("lib",)), "missing: lib/*"),
    (lambda b: _info(b, app_version="1.2.2"), "app_version '1.2.2'"),
    (lambda b: _info(b, binary_file_type="ELF 64-bit LSB pie executable, x86-64"), "not a 32-bit ARM"),
    (lambda b: (b / "main").write_bytes(b"other"), "does not match binary_sha256"),
])
def test_wrong_build_refused(build, tmp_path, change, message):
    change(build)
    with pytest.raises(SystemExit) as exc:
        mr.package(build, tmp_path / "release", "1.2.3")
    assert message in str(exc.value)
    assert not (tmp_path / "release").exists()


def _info(build, **changes):
    info = json.loads((build / "build_info.json").read_text())
    info.update(changes)
    (build / "build_info.json").write_text(json.dumps(info))


def test_publish_uploads_every_file_and_creates_release(build, tmp_path, monkeypatch):
    files = mr.package(build, tmp_path / "release", "1.2.3")
    calls = []
    monkeypatch.setattr(mr, "api", lambda method, url, data=None, ct=None: calls.append((method, url, data)) or (201, b""))
    monkeypatch.setenv("CI_API_V4_URL", "http://gitlab/api/v4")
    monkeypatch.setenv("CI_PROJECT_ID", "15")
    monkeypatch.setenv("CI_COMMIT_TAG", "v1.2.3")
    mr.publish(files, "1.2.3")
    puts = [c for c in calls if c[0] == "PUT"]
    assert len(puts) == len(files)
    assert all(u.startswith("http://gitlab/api/v4/projects/15/packages/generic/stm32mp1-gateway/1.2.3/") for _, u, _ in puts)
    (method, url, data), = [c for c in calls if c[0] == "POST"]
    assert url == "http://gitlab/api/v4/projects/15/releases"
    body = json.loads(data)
    assert body["tag_name"] == "v1.2.3" and len(body["assets"]["links"]) == len(files)


def test_dry_run_and_no_tag_never_publish(build, tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "app_version", lambda: "1.2.3")
    monkeypatch.setattr(mr, "publish", lambda *a: pytest.fail("must not publish"))
    monkeypatch.delenv("CI_COMMIT_TAG", raising=False)
    mr.main([str(build), str(tmp_path / "r1")])
    monkeypatch.setenv("CI_COMMIT_TAG", "v1.2.3")
    mr.main([str(build), str(tmp_path / "r2"), "--dry-run"])
    assert (tmp_path / "r2" / "SHA256SUMS").exists()
