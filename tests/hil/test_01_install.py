"""The CI build installed with the production installer (deploy/install.sh)."""

import hashlib
import re

import pytest

from board import BINARY, CONFIG

pytestmark = pytest.mark.hardware


def test_installer_reports_running_version(board, build_info):
    assert f"gateway {build_info['app_version']} running" in board.install_log


def test_installed_binary_is_the_ci_build(board, build_info):
    sha = board.run(f"sha256sum {BINARY}").out.split()[0]
    assert sha == build_info["binary_sha256"]
    assert board.run(f"{BINARY} --version").out.strip() == build_info["app_version"]
    assert board.run("cat /opt/gateway/VERSION").out.strip() == build_info["app_version"]


def test_service_enabled_and_running(board):
    assert board.run("systemctl is-enabled gateway").out.strip() == "enabled"
    assert board.is_active()


def test_service_uses_production_paths(board):
    cmd = board.run("tr '\\0' ' ' < /proc/$(systemctl show gateway -p MainPID --value)/cmdline").out
    assert cmd.split() == [BINARY, "--config", CONFIG, "--data-dir", "/var/lib/gateway"]


def test_bundled_libraries_used_not_os_copies(board):
    """RPATH $ORIGIN/../lib: libmodbus/libmosquitto/OpenSSL come from /opt/gateway/lib."""
    maps = board.run("cat /proc/$(systemctl show gateway -p MainPID --value)/maps").out
    libs = set(re.findall(r"(/\S+\.so[.\d]*)$", maps, re.M))
    for name in ("libmodbus.so", "libmosquitto.so", "libssl.so", "libcurl.so"):
        paths = [p for p in libs if p.rsplit("/", 1)[1].startswith(name)]
        assert paths and all(p.startswith("/opt/gateway/lib/") for p in paths), (name, paths)


def test_secret_files_not_world_readable(board):
    out = board.run("stat -c '%a %n' /etc/gateway/certs /etc/gateway/smart_rtu_config.json").out
    modes = dict(line.split(" ", 1)[::-1] for line in out.strip().splitlines())
    assert modes["/etc/gateway/certs"] == "700"
    assert modes["/etc/gateway/smart_rtu_config.json"] == "600"


def test_help_and_bad_options(board):
    assert "--data-dir" in board.run(f"{BINARY} --help").out
    r = board.run(f"{BINARY} --no-such-option", check=False)
    assert r.rc == 2 and "Usage" in r.err
