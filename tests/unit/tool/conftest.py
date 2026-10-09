"""Fixtures for the Smart RTU tool tests (tools/smart_rtu_tool).

Everything here is automatic (autouse) unless noted, so no test can touch the
real world by accident:

* Qt runs headless (QT_QPA_PLATFORM=offscreen); ``qapp`` gives the QApplication.
* ``dialogs``: every QMessageBox / QFileDialog call in ui.main_window is
  recorded instead of shown (a real modal dialog would hang CI). Set
  ``dialogs.answer`` (QMessageBox button), ``dialogs.open_path`` and
  ``dialogs.save_path`` to "click" them.
* The OS keyring is replaced by an in-memory backend (your real stored
  passwords are never read or written).
* The offline retry queue lives in a temporary directory.
* Serial port listing returns no ports; opening a real serial port, SSH
  connection or MQTT client fails the test.
"""

import os
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
TOOL = REPO / "tools" / "smart_rtu_tool"
sys.path.insert(0, str(TOOL))
sys.path.insert(0, str(Path(__file__).parent))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import keyring                       # noqa: E402
from keyring.backend import KeyringBackend   # noqa: E402
from keyring.errors import PasswordDeleteError   # noqa: E402


class MemoryKeyring(KeyringBackend):
    """In-memory keyring: {(service, key): value}."""
    priority = 1

    def __init__(self):
        super().__init__()
        self.store = {}

    def get_password(self, service, username):
        return self.store.get((service, username))

    def set_password(self, service, username, password):
        self.store[(service, username)] = password

    def delete_password(self, service, username):
        if (service, username) not in self.store:
            raise PasswordDeleteError("not found")
        del self.store[(service, username)]


@pytest.fixture(autouse=True)
def memory_keyring():
    previous = keyring.get_keyring()
    ring = MemoryKeyring()
    keyring.set_keyring(ring)
    yield ring
    keyring.set_keyring(previous)


@pytest.fixture(autouse=True)
def queue_dir(tmp_path, monkeypatch):
    from core import offline_queue
    d = tmp_path / "pending_configs"
    monkeypatch.setattr(offline_queue, "QUEUE_DIR", str(d))
    return d


@pytest.fixture(autouse=True)
def no_real_io(monkeypatch):
    """Fail loudly if a test reaches real hardware or network."""
    import paho.mqtt.client as mqtt
    import paramiko
    import serial
    import serial.tools.list_ports

    def forbidden(what):
        def _raise(*a, **k):
            raise AssertionError(f"test tried to open a real {what}; use the fakes in fakes.py")
        return _raise

    monkeypatch.setattr(serial, "Serial", forbidden("serial port"))
    monkeypatch.setattr(serial.tools.list_ports, "comports", lambda: [])
    monkeypatch.setattr(paramiko, "SSHClient", forbidden("SSH connection"))
    monkeypatch.setattr(mqtt, "Client", forbidden("MQTT client"))


@pytest.fixture(scope="session")
def qapp():
    from PyQt5.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    yield app


class Dialogs:
    """Records message boxes and file dialogs; answers them from attributes."""

    def __init__(self):
        from PyQt5.QtWidgets import QMessageBox
        self.calls = []              # (kind, title, text)
        self.answer = QMessageBox.Yes
        self.open_path = ""
        self.save_path = ""

    def kinds(self):
        return [c[0] for c in self.calls]

    def texts(self):
        return [c[2] for c in self.calls]


@pytest.fixture
def dialogs(monkeypatch, qapp):
    from PyQt5.QtWidgets import QFileDialog, QMessageBox
    import ui.main_window as mw

    rec = Dialogs()

    def box(kind):
        def _show(parent, title, text, *a, **k):
            rec.calls.append((kind, title, text))
            return rec.answer if kind == "question" else QMessageBox.Ok
        return staticmethod(_show)

    class FakeMessageBox(QMessageBox):
        information = box("information")
        warning = box("warning")
        critical = box("critical")
        question = box("question")

    class FakeFileDialog(QFileDialog):
        @staticmethod
        def getOpenFileName(parent, title, *a, **k):
            rec.calls.append(("open", title, ""))
            return rec.open_path, ""

        @staticmethod
        def getSaveFileName(parent, title, *a, **k):
            rec.calls.append(("save", title, ""))
            return rec.save_path, ""

    monkeypatch.setattr(mw, "QMessageBox", FakeMessageBox)
    monkeypatch.setattr(mw, "QFileDialog", FakeFileDialog)
    return rec


@pytest.fixture
def window(qapp, dialogs):
    """A MainWindow; closed and deleted after the test."""
    from ui.main_window import MainWindow
    w = MainWindow()
    yield w
    w.close()
    w.deleteLater()
    qapp.processEvents()
