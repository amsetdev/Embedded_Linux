"""core/ssh_transport.py against a fake board (fakes.FakeBoardFS)."""

import paramiko
import pytest

import core.ssh_transport as ssh_mod
from core import REMOTE_CONFIG_CSV_PATH, REMOTE_CONFIG_DIR, REMOTE_CONFIG_JSON_PATH
from core.ssh_transport import SSHTransport
from fakes import FakeBoardFS, ssh_client_factory

pytestmark = pytest.mark.unit


@pytest.fixture
def board(monkeypatch):
    fs = FakeBoardFS()
    monkeypatch.setattr(paramiko, "SSHClient", ssh_client_factory(fs))
    return fs


@pytest.fixture
def ssh():
    return SSHTransport("192.0.2.10", "root", "pw", port=2222, timeout=3)


def test_connects_with_given_credentials(board, ssh):
    assert ssh.test_connection().ok
    assert board.connections == [{"host": "192.0.2.10", "port": 2222, "username": "root",
                                  "password": "pw", "timeout": 3}]


def test_connection_failure_reported_not_raised(board, ssh):
    board.refuse = OSError("No route to host")
    r = ssh.test_connection()
    assert not r.ok and "No route to host" in r.message


def test_push_text_writes_tmp_then_renames(board, ssh):
    r = ssh.push_text("hello\n", "/home/root/x.json")
    assert r.ok
    assert board.files == {"/home/root/x.json": b"hello\n"}
    assert board.commands == ["mv /home/root/x.json.tmp /home/root/x.json"]


def test_push_text_reports_rename_failure(board, ssh, monkeypatch):
    monkeypatch.setattr(board, "run", lambda cmd: ("", "mv: Read-only file system"))
    r = ssh.push_text("x", "/ro/file")
    assert not r.ok and "Read-only file system" in r.message


def test_push_combined_config_writes_both_files(board, ssh):
    r = ssh.push_combined_config("csv-content", "{}")
    assert r.ok and REMOTE_CONFIG_DIR in r.message
    assert board.files == {REMOTE_CONFIG_CSV_PATH: b"csv-content", REMOTE_CONFIG_JSON_PATH: b"{}"}


def test_push_combined_config_stops_after_first_failure(board, ssh, monkeypatch):
    calls = []
    monkeypatch.setattr(ssh, "push_text", lambda c, p: calls.append(p) or ssh_mod.TransportResult(False, "boom"))
    assert not ssh.push_combined_config("a", "b").ok
    assert calls == [REMOTE_CONFIG_CSV_PATH]


def test_push_file_uploads_bytes(board, ssh, tmp_path):
    local = tmp_path / "ca.crt"
    local.write_bytes(b"\x00\x01PEM")
    assert ssh.push_file(str(local), "/home/root/ca.crt").ok
    assert board.files["/home/root/ca.crt"] == b"\x00\x01PEM"


def test_push_file_missing_local_file(board, ssh, tmp_path):
    r = ssh.push_file(str(tmp_path / "nope.crt"), "/x")
    assert not r.ok and "Cannot read local file" in r.message
    assert board.connections == []


def test_read_text_returns_content(board, ssh):
    board.files[REMOTE_CONFIG_JSON_PATH] = b'{"device": {}}'
    r = ssh.read_text(REMOTE_CONFIG_JSON_PATH)
    assert r.ok and r.message == '{"device": {}}'


def test_read_text_missing_file(board, ssh):
    r = ssh.read_text(REMOTE_CONFIG_JSON_PATH)
    assert not r.ok and "No file found" in r.message


def test_delete_remote(board, ssh):
    board.files["/a"] = b"x"
    assert ssh.delete_remote("/a").ok
    assert "/a" not in board.files


def test_config_written_to_production_paths():
    assert REMOTE_CONFIG_JSON_PATH == "/etc/gateway/smart_rtu_config.json"
    assert REMOTE_CONFIG_CSV_PATH == "/etc/gateway/smart_rtu_config.csv"


def test_restart_restarts_the_systemd_service(board, ssh):
    r = ssh.restart_app()
    assert r.ok and "restarted" in r.message
    assert board.commands == ["systemctl restart gateway"]


def test_restart_failure_reported(board, ssh):
    board.service_fails = True
    r = ssh.restart_app()
    assert not r.ok and "gateway.service failed" in r.message


def test_log_tail_from_journal(board, ssh):
    board.journal = b"line1\nline2\n"
    r = ssh.read_remote_log_tail(5)
    assert r.ok and r.message == "line1\nline2\n"
    assert board.commands == ["journalctl -u gateway -n 5 --no-pager -o cat"]
