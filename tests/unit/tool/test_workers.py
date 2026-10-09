"""The background workers in ui/main_window.py: what each delivery mode does.

Workers are QThreads; tests call run() directly (same thread, signals delivered
synchronously) with ui.main_window._make_transport returning a FakeTransport.
"""

import pytest

import ui.main_window as mw
from core import (REMOTE_CA_CERT_PATH, REMOTE_CONFIG_CSV_PATH, REMOTE_CONFIG_JSON_PATH,
                  offline_queue)
from core.register_model import DeviceConfig, RegisterPoint
from core.transport import TransportResult
from fakes import FakeTransport

pytestmark = pytest.mark.unit

FAIL = TransportResult(False, "unreachable")
SSH = {"host": "192.0.2.10", "username": "root", "password": "pw"}
SERIAL = {"port": "/dev/ttyACM0", "console_baud": 115200, "username": "admin", "password": "pw"}
MQTT = {"broker": "b", "port": 8883, "username": "u", "password": "p", "ack_timeout": 3}


@pytest.fixture
def fake(monkeypatch, qapp):
    t = FakeTransport()
    made = []
    monkeypatch.setattr(mw, "_make_transport", lambda mode, params: made.append((mode, params)) or t)
    t.made = made
    return t


def config():
    return DeviceConfig(device_id="GW-1", points=[RegisterPoint("A", 0, slave_id=1)])


def run_push(mode, params, cert_pairs=None):
    w = mw.PushWorker(mode, config(), params, cert_pairs=cert_pairs)
    out = []
    w.finished_ok.connect(lambda m: out.append(("ok", m)))
    w.finished_fail.connect(lambda m: out.append(("fail", m)))
    w.run()
    (result,) = out
    return result


def test_make_transport_builds_each_kind():
    assert isinstance(mw._make_transport("mqtt", MQTT), mw.MQTTTransport)
    s = mw._make_transport("ssh", SSH)
    assert isinstance(s, mw.SSHTransport) and (s.host, s.username) == ("192.0.2.10", "root")
    t = mw._make_transport("serial", SERIAL)
    assert isinstance(t, mw.SerialTransport) and (t.port, t.baud) == ("/dev/ttyACM0", 115200)


def test_ssh_push_writes_config_then_restarts_app(fake):
    kind, msg = run_push("ssh", SSH)
    assert kind == "ok"
    assert fake.names() == ["push_combined_config", "restart_app"]
    csv, js = fake.calls[0][1]
    assert csv == config().to_full_config_csv_string() and js == config().to_full_config_json()


def test_ssh_push_failure_does_not_restart(fake):
    fake.results["push_combined_config"] = FAIL
    kind, msg = run_push("ssh", SSH)
    assert kind == "fail" and "unreachable" in msg
    assert fake.names() == ["push_combined_config"]


def test_ssh_restart_failure_reported(fake):
    fake.results["restart_app"] = FAIL
    kind, msg = run_push("ssh", SSH)
    assert kind == "fail" and "Restart failed" in msg


def test_serial_push_passes_console_login(fake):
    assert run_push("serial", SERIAL)[0] == "ok"
    (name, args, kwargs), = fake.calls
    assert kwargs == {"username": "admin", "password": "pw"}


def test_mqtt_push_ok(fake):
    assert run_push("mqtt", MQTT)[0] == "ok"
    (name, args, kwargs), = fake.calls
    assert args[0] == "GW-1" and kwargs == {"wait_ack_seconds": 3}
    assert offline_queue.list_queued() == []


def test_mqtt_push_failure_queues_config(fake):
    fake.results["push_combined_config"] = FAIL
    kind, msg = run_push("mqtt", MQTT)
    assert kind == "fail" and "queued locally" in msg
    (item,) = offline_queue.list_queued()
    assert item.device_id == "GW-1" and item.csv_content == config().to_full_config_csv_string()
    assert item.json_content == config().to_full_config_json()


def test_certificates_uploaded_before_config(fake, tmp_path):
    ca = tmp_path / "ca.crt"
    ca.write_text("PEM")
    kind, msg = run_push("ssh", SSH, cert_pairs=[(str(ca), REMOTE_CA_CERT_PATH)])
    assert kind == "ok" and "Uploaded certificates: ca.crt" in msg
    assert fake.names() == ["push_file", "push_combined_config", "restart_app"]
    assert fake.calls[0][1] == (str(ca), REMOTE_CA_CERT_PATH)


def test_certificate_upload_failure_stops_push(fake, tmp_path):
    fake.results["push_file"] = FAIL
    kind, msg = run_push("ssh", SSH, cert_pairs=[(str(tmp_path / "k.key"), "/r/k.key")])
    assert kind == "fail" and "Failed to upload k.key" in msg
    assert fake.names() == ["push_file"]


def test_mqtt_cannot_upload_certificates_says_so(fake, tmp_path):
    kind, msg = run_push("mqtt", MQTT, cert_pairs=[(str(tmp_path / "ca.crt"), REMOTE_CA_CERT_PATH)])
    assert kind == "ok" and "placed on the board manually" in msg
    assert "push_file" not in fake.names()


def test_unexpected_exception_becomes_failure(monkeypatch, qapp):
    def boom(mode, params):
        raise RuntimeError("driver crashed")
    monkeypatch.setattr(mw, "_make_transport", boom)
    kind, msg = run_push("ssh", SSH)
    assert kind == "fail" and "driver crashed" in msg


@pytest.mark.parametrize("mode, params, action, expected", [
    ("ssh", SSH, "read", [("read_text", (REMOTE_CONFIG_JSON_PATH,))]),
    ("ssh", SSH, "erase", [("delete_remote", (REMOTE_CONFIG_CSV_PATH,)), ("delete_remote", (REMOTE_CONFIG_JSON_PATH,))]),
    ("serial", SERIAL, "read", [("read_text", (REMOTE_CONFIG_JSON_PATH, "admin", "pw"))]),
    ("serial", SERIAL, "erase", [("delete_remote", (REMOTE_CONFIG_CSV_PATH, "admin", "pw")),
                                 ("delete_remote", (REMOTE_CONFIG_JSON_PATH, "admin", "pw"))]),
    ("mqtt", MQTT, "read", [("read_text", ("amset/GW-1/config/set",))]),
    ("mqtt", MQTT, "erase", [("delete_remote", ("amset/GW-1/config/set",))]),
])
def test_settings_read_and_erase_targets(fake, mode, params, action, expected):
    w = mw.ConfigFieldWorker(mode, action, config(), params)
    out = []
    w.finished.connect(lambda ok, m: out.append((ok, m)))
    w.run()
    assert out[0][0] is True
    assert [(n, a) for n, a, k in fake.calls] == expected


def test_settings_send_does_not_restart_app(fake):
    """Regression guard: 'Send Settings' only writes the files (no restart), unlike 'Push Data'."""
    w = mw.ConfigFieldWorker("ssh", "send", config(), SSH)
    w.finished.connect(lambda ok, m: None)
    w.run()
    assert fake.names() == ["push_combined_config"]


def test_erase_stops_when_first_delete_fails(fake):
    fake.results["delete_remote"] = FAIL
    w = mw.ConfigFieldWorker("ssh", "erase", config(), SSH)
    out = []
    w.finished.connect(lambda ok, m: out.append(ok))
    w.run()
    assert out == [False] and fake.names() == ["delete_remote"]


def test_retry_queue_sends_json_and_removes_only_delivered(fake, qapp):
    offline_queue.enqueue("GW-1", "csv-1", "json-1")
    offline_queue.enqueue("GW-2", "csv-2", "json-2")
    fake.results["push_combined_config"] = lambda dev, csv, js, **k: TransportResult(dev == "GW-1", "x")
    w = mw.RetryQueueWorker(MQTT)
    out = []
    w.finished.connect(out.append)
    w.run()
    assert [(a, k) for n, a, k in fake.calls] == [(("GW-1", "csv-1", "json-1"), {"wait_ack_seconds": 3}),
                                                  (("GW-2", "csv-2", "json-2"), {"wait_ack_seconds": 3})]
    assert fake.made[0][0] == "mqtt"
    assert [i.device_id for i in offline_queue.list_queued()] == ["GW-2"]
    assert out[0] == ["GW-1: delivered", "GW-2: still pending (x)"]


def test_retry_keeps_old_csv_only_entries(fake, qapp):
    offline_queue.enqueue("GW-1", "csv-1")
    w = mw.RetryQueueWorker(MQTT)
    out = []
    w.finished.connect(out.append)
    w.run()
    assert fake.calls == [] and "push it again" in out[0][0]
    assert len(offline_queue.list_queued()) == 1


def test_mqtt_certificate_fields_reach_transport():
    t = mw._make_transport("mqtt", {**MQTT, "ca_certs": "/ca", "certfile": "/crt", "keyfile": "/key"})
    assert (t.ca_certs, t.certfile, t.keyfile) == ("/ca", "/crt", "/key")
