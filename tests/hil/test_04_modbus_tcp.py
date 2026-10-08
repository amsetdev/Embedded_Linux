"""Modbus TCP thread (src/fieldbus/mb_tcp.c) against the TCP slave on this PC, reached
by the board through the SSH connection (127.0.0.1:BOARD_TCP_PORT on the board):
100 holding registers stored in /var/lib/gateway/modbus_tcp.db."""

import sqlite3

import pytest

from conftest import BOARD_TCP_PORT, base_config

pytestmark = pytest.mark.hardware


@pytest.fixture(scope="module")
def tcp_polled(gateway, slaves):
    gateway.b.run("rm -f /var/lib/gateway/modbus_tcp.db")
    mark = gateway.apply_config(base_config([], tcp=True))
    gateway.b.wait_log(mark, rf"Connected to slave 127\.0\.0\.1:{BOARD_TCP_PORT}", timeout=60)
    gateway.b.wait_log(mark, r"\[ MODBUS_TCP \] 100 registers read and stored", timeout=60)
    return mark


def test_registers_stored_in_data_dir_database(gateway, tcp_polled, tmp_path):
    # Read on this PC: the board's sqlite3 CLI doesn't run (its OS libsqlite3 was replaced
    # by another version, DOCS/CI_CD_GUIDE.md §6).
    local = tmp_path / "modbus_tcp.db"
    sftp = gateway.b.ssh.open_sftp()
    try:
        sftp.get("/var/lib/gateway/modbus_tcp.db", str(local))
    finally:
        sftp.close()
    with sqlite3.connect(local) as db:
        rows = db.execute("SELECT address, value FROM holding_registers ORDER BY address LIMIT 100").fetchall()
    assert rows[:3] == [(1, 1000), (2, 1001), (3, 1002)]          # address N = register N-1
    assert rows[-1] == (100, 1099)


def test_database_in_data_dir(gateway, tcp_polled):
    """It used to be /tmp/modbus_data.db (lost at every reboot)."""
    assert "Initialised → /var/lib/gateway/modbus_tcp.db" in gateway.b.since(tcp_polled)
