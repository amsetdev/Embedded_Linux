"""core/serial_transport.py against a fake login console (fakes.FakeConsole).

A real serial console ECHOES everything typed. Tests run with echo on (the real
case) and, where it shows the logic itself works, with echo off.
"""

import json

import pytest
import serial

import core.serial_transport as serial_mod
from core import REMOTE_CONFIG_CSV_PATH, REMOTE_CONFIG_JSON_PATH
from core.register_model import DeviceConfig, RegisterPoint
from core.serial_transport import SerialTransport
from fakes import FakeClock, FakeConsole

pytestmark = pytest.mark.unit


@pytest.fixture
def clock(monkeypatch):
    c = FakeClock()
    monkeypatch.setattr(serial_mod, "time", c)
    return c


@pytest.fixture
def console(monkeypatch, clock):
    """Factory: console(**FakeConsole kwargs) installs it as the serial port."""
    holder = {}

    def make(**kwargs):
        con = FakeConsole(**kwargs)
        holder["con"] = con
        monkeypatch.setattr(serial, "Serial", lambda *a, **k: con)
        return con
    return make


def tool_json():
    return DeviceConfig(points=[RegisterPoint("A", 0, slave_id=1)]).to_full_config_json()


def tool_csv():
    return DeviceConfig(points=[RegisterPoint("A", 0, slave_id=1)]).to_full_config_csv_string()


def test_open_failure_reported(monkeypatch, clock):
    def refuse(*a, **k):
        raise serial.SerialException("could not open port /dev/ttyACM9")
    monkeypatch.setattr(serial, "Serial", refuse)
    r = SerialTransport("/dev/ttyACM9").test_connection()
    assert not r.ok and "ttyACM9" in r.message


def test_logs_in_with_username_and_password(console):
    con = console(echo=False, login=("root", "s3cret"))
    SerialTransport("/dev/ttyACM0").push_text("x\n", "/tmp/f", username="root", password="s3cret")
    assert con.state == "shell"
    assert b"root\r\n" in con.typed and b"s3cret\r\n" in con.typed


def test_push_text_without_echo_writes_file(console):
    con = console(echo=False)
    r = SerialTransport("/dev/ttyACM0").push_text("a,b\nc,d\n", "/home/root/f.csv")
    assert r.ok
    assert con.files == {"/home/root/f.csv": "a,b\nc,d\n"}


def test_push_csv_with_echo_writes_file(console):
    """The CSV export ends with a newline, so the heredoc terminator is on its own line."""
    con = console()
    r = SerialTransport("/dev/ttyACM0").push_text(tool_csv(), REMOTE_CONFIG_CSV_PATH)
    assert r.ok
    assert con.files[REMOTE_CONFIG_CSV_PATH] == tool_csv().replace("\r\n", "\n")


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: the JSON has no trailing newline, so 'EOF' is appended to its last line ('}EOF'), "
    "the heredoc never ends and the file is not written; with console echo the typed marker still "
    "reports success"))
def test_push_json_writes_file(console):
    con = console()
    SerialTransport("/dev/ttyACM0").push_text(tool_json(), REMOTE_CONFIG_JSON_PATH)
    assert json.loads(con.files[REMOTE_CONFIG_JSON_PATH]) == json.loads(tool_json())


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: the completion marker is part of the typed command, and the console echoes it, so a "
    "failed 'mv' is reported as success"))
def test_failed_write_not_reported_as_success(console):
    console(fail_mv=True)
    r = SerialTransport("/dev/ttyACM0").push_text("x\n", "/readonly/f")
    assert not r.ok


def test_failed_write_detected_without_echo(console, clock):
    console(echo=False, fail_mv=True)
    r = SerialTransport("/dev/ttyACM0").push_text("x\n", "/readonly/f")
    assert not r.ok and "No completion marker" in r.message
    assert clock.slept >= 10, "waits for the 10 s deadline"


def test_read_text_without_echo(console):
    console(echo=False, files={REMOTE_CONFIG_JSON_PATH: '{"a": 1}\n'})
    r = SerialTransport("/dev/ttyACM0").read_text(REMOTE_CONFIG_JSON_PATH)
    assert r.ok and r.message.strip() == '{"a": 1}'


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: the start/end markers are in the typed command, which the console echoes, so "
    "read_text() returns the echoed command instead of the file"))
def test_read_text_with_echo(console):
    console(files={REMOTE_CONFIG_JSON_PATH: '{"a": 1}\n'})
    r = SerialTransport("/dev/ttyACM0").read_text(REMOTE_CONFIG_JSON_PATH)
    assert r.ok and r.message.strip() == '{"a": 1}'


def test_read_text_missing_file_without_echo(console):
    console(echo=False)
    r = SerialTransport("/dev/ttyACM0").read_text("/nope")
    assert not r.ok and "No file found" in r.message


def test_delete_remote_without_echo(console):
    con = console(echo=False, files={"/a": "x\n"})
    assert SerialTransport("/dev/ttyACM0").delete_remote("/a").ok
    assert con.files == {}


def test_push_combined_config_order_and_stop_on_failure(monkeypatch):
    t = SerialTransport("/dev/ttyACM0")
    calls = []
    monkeypatch.setattr(t, "push_text", lambda c, p, u, pw: calls.append((p, u, pw)) or serial_mod.TransportResult(False, "x"))
    assert not t.push_combined_config("csv", "json", "admin", "pw").ok
    assert calls == [(REMOTE_CONFIG_CSV_PATH, "admin", "pw")]


def test_port_closed_after_every_operation(console):
    con = console(echo=False)
    SerialTransport("/dev/ttyACM0").push_text("x\n", "/f")
    assert not con.is_open
