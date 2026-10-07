"""core/mqtt_transport.py against a fake broker (fakes.FakeBroker).

Whether the firmware listens on these topics at all is a contract question:
tests/validate/test_tool_contract.py::test_firmware_subscribes_to_tool_config_topic.
"""

import json

import paho.mqtt.client as mqtt
import pytest

import core.mqtt_transport as mqtt_mod
from core.mqtt_transport import MQTTTransport
from fakes import FakeBroker, FakeClock

pytestmark = pytest.mark.unit


@pytest.fixture
def broker(monkeypatch):
    b = FakeBroker()
    monkeypatch.setattr(mqtt, "Client", b.client_factory())
    monkeypatch.setattr(mqtt_mod, "time", FakeClock())
    return b


@pytest.fixture
def transport():
    return MQTTTransport("broker.example", 8883, "user", "pw")


def ack_with(status, registers=3, **extra):
    return lambda payload: ("amset/GW-1/config/ack",
                            json.dumps({"status": status, "registers": registers, **extra}).encode())


def test_connection_ok_uses_tls_and_credentials(broker, transport):
    assert transport.test_connection().ok
    (c,) = broker.clients
    assert c.connected_to == ("broker.example", 8883)
    assert c.credentials == ("user", "pw")
    assert c.tls is not None


def test_no_tls_when_disabled(broker):
    MQTTTransport("b", 1883, "u", "p", use_tls=False).test_connection()
    assert broker.clients[0].tls is None


def test_client_certificate_auth_for_aws_iot(broker):
    MQTTTransport("x-ats.iot.example", 8883, "", "", ca_certs="/c/ca.pem",
                  certfile="/c/dev.crt", keyfile="/c/dev.key").test_connection()
    c = broker.clients[0]
    assert c.credentials is None                     # no username/password with mTLS
    assert (c.tls["ca_certs"], c.tls["certfile"], c.tls["keyfile"]) == ("/c/ca.pem", "/c/dev.crt", "/c/dev.key")


@pytest.mark.parametrize("rc, text", [(4, "bad credentials"), (5, "not authorised"), (3, "broker unavailable")])
def test_connack_refusal_explained(broker, transport, rc, text):
    broker.connack_rc = rc
    r = transport.test_connection()
    assert not r.ok and text in r.message


def test_connack_timeout(broker, transport, monkeypatch):
    broker.never_connack = True
    import threading
    monkeypatch.setattr(threading.Event, "wait", lambda self, timeout=None: False)
    r = transport.test_connection()
    assert not r.ok and "Timed out" in r.message


def test_push_text_succeeds_only_with_ack(broker, transport):
    broker.responders["amset/GW-1/config/set"] = ack_with("saved", 42)
    r = transport.push_text("{}", "amset/GW-1/config/set", ack_topic="amset/GW-1/config/ack")
    assert r.ok and "42 registers" in r.message and r.ack_payload["status"] == "saved"
    (_, topic, payload, qos, retain) = broker.published[0]
    assert (topic, payload, qos, retain) == ("amset/GW-1/config/set", b"{}", 1, True)


def test_unchanged_ack_is_success(broker, transport):
    broker.responders["amset/GW-1/config/set"] = ack_with("unchanged")
    r = transport.push_text("{}", "amset/GW-1/config/set", ack_topic="amset/GW-1/config/ack")
    assert r.ok and "already up to date" in r.message


@pytest.mark.parametrize("status", ["rejected", "error"])
def test_rejected_or_failed_ack_is_failure(broker, transport, status):
    broker.responders["amset/GW-1/config/set"] = ack_with(status, error="not_a_config")
    r = transport.push_text("{}", "amset/GW-1/config/set", ack_topic="amset/GW-1/config/ack")
    assert not r.ok and status in r.message and "not_a_config" in r.message


def test_push_text_without_ack_fails_but_stays_retained(broker, transport):
    r = transport.push_text("csv", "amset/GW-1/config/set", ack_topic="amset/GW-1/config/ack", wait_ack_seconds=2)
    assert not r.ok and "no acknowledgment" in r.message
    assert broker.retained["amset/GW-1/config/set"] == b"csv"


def test_non_json_ack_still_counts(broker, transport):
    broker.responders["t/set"] = lambda p: ("t/ack", b"OK")
    r = transport.push_text("x", "t/set", ack_topic="t/ack")
    assert r.ok and r.ack_payload == {"raw": "OK"}


def test_push_combined_config_sends_the_json_the_firmware_reads(broker, transport):
    """Regression: the CSV went to config/set (and the JSON to set.json after the ack wait),
    while the firmware reads the JSON (inc/config_push.h)."""
    broker.responders["amset/GW-1/config/set"] = ack_with("saved", 1)
    r = transport.push_combined_config("GW-1", "csv-data", '{"j": 1}')
    assert r.ok
    assert broker.retained == {"amset/GW-1/config/set": b'{"j": 1}'}


def test_read_text_returns_retained(broker, transport):
    broker.retained["amset/GW-1/config/set"] = b"stored"
    r = transport.read_text("amset/GW-1/config/set")
    assert r.ok and r.message == "stored"


def test_read_text_nothing_retained(broker, transport):
    r = transport.read_text("amset/GW-1/config/set", timeout=1)
    assert not r.ok and "No retained message" in r.message


def test_delete_clears_retained(broker, transport):
    broker.retained["t"] = b"x"
    assert transport.delete_remote("t").ok
    assert "t" not in broker.retained
    assert broker.published[-1][2:] == (b"", 1, True)


def test_push_file_not_supported(transport):
    r = transport.push_file("/local/ca.crt", "/remote/ca.crt")
    assert not r.ok and "cannot upload files" in r.message
