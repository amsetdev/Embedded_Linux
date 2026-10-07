"""AWS IoT (skipped without the HIL_AWS_* variables, aws_link.py): telemetry, config
push over MQTT, OTA command checks, replay of buffered payloads after PUBACK."""

import json
import os

import pytest

from aws_link import Observer, endpoint, skip_without_network
from board import CERTS
from conftest import DEVICE_ID, RTU_REGISTERS, base_config, rtu_available

pytestmark = pytest.mark.hardware

TELEMETRY = f"hil/{DEVICE_ID}/telemetry"
ACK = f"amset/{DEVICE_ID}/config/ack"
SET = f"amset/{DEVICE_ID}/config/set"
OTA_APP = f"devices/{DEVICE_ID}/ota/app"
OTA_STATUS = f"devices/{DEVICE_ID}/ota/status"


def mqtt_section():
    return {"broker": endpoint(), "port": 8883, "client_id": DEVICE_ID,
            "ca_cert": f"{CERTS}/hil-ca.crt", "device_cert": f"{CERTS}/hil-device.crt",
            "private_key": f"{CERTS}/hil-device.key", "topic": TELEMETRY}


@pytest.fixture(scope="module")
def online(gateway, slaves):
    skip_without_network()
    b = gateway.b
    for src, name in (("HIL_AWS_CA", "hil-ca.crt"), ("HIL_DEVICE_CERT", "hil-device.crt"),
                      ("HIL_DEVICE_KEY", "hil-device.key")):
        b.put(open(os.environ[src], "rb").read(), f"{CERTS}/{name}", mode=0o600)
    observer = Observer([TELEMETRY, ACK, OTA_STATUS])
    regs = [r for r, _ in RTU_REGISTERS] if rtu_available() else []
    cfg = base_config(regs, mqtt=mqtt_section())
    cfg["ota"] = {"enable": 1, "app_topic": OTA_APP, "system_topic": f"devices/{DEVICE_ID}/ota/system",
                  "status_topic": OTA_STATUS, "download_dir": "/var/lib/gateway/ota",
                  "app_version": gateway.build_info["app_version"], "app_binary_path": "/opt/gateway/bin/gateway"}
    mark = gateway.apply_config(cfg)
    b.wait_log(mark, r"\[MQTT\] Connected to AWS IoT Core", timeout=60)
    try:
        yield observer, cfg, mark
    finally:
        observer.publish(SET, b"", retain=True)        # never leave a retained test config behind
        observer.close()


def test_telemetry_published(online):
    observer, cfg, _ = online
    msg = observer.wait_json(TELEMETRY, lambda d: "values" in d, timeout=60)
    assert set(msg) == {"ts", "values"}
    if cfg["registers"]:
        assert set(msg["values"]) == {r["label"] for r in cfg["registers"]}


def test_config_push_over_mqtt_saved_and_applied(online, gateway):
    observer, cfg, _ = online
    new = json.loads(json.dumps(cfg))
    new["device"]["interval_sec"] = 9
    mark = gateway.b.mark()
    observer.publish(SET, json.dumps(new, indent=2), retain=True)
    ack = observer.wait_json(ACK, lambda d: d.get("status") in ("saved", "unchanged", "rejected", "error"), timeout=30)
    assert ack == {"status": "saved", "registers": len(new["registers"])}
    gateway.b.wait_log(mark, r"Interval       : 9 sec", timeout=60)
    assert json.loads(gateway.b.read("/etc/gateway/smart_rtu_config.json"))["device"]["interval_sec"] == 9


def test_config_push_rejects_garbage(online):
    observer, _, _ = online
    observer.publish(SET, b"not a config", retain=False)
    ack = observer.wait_json(ACK, lambda d: d.get("status") == "rejected", timeout=30)
    assert ack["error"] == "not_a_config"


@pytest.mark.parametrize("command, error", [
    ({"url": "https://example.invalid/gw", "version": "9.9.9"}, "invalid_sha256"),
    ({"url": "http://example.invalid/gw", "version": "9.9.9", "sha256": "0" * 64}, "invalid_url"),
])
def test_unsafe_ota_command_rejected(online, command, error):
    observer, _, _ = online
    observer.publish(OTA_APP, json.dumps(command))
    status = observer.wait_json(OTA_STATUS, lambda d: d.get("error") == error, timeout=30)
    assert status["status"] == "FAILED" and status["type"] == "app"


def test_buffered_payloads_replayed_and_deleted(online, gateway):
    """Payloads stored while offline arrive on the telemetry topic; each file is deleted
    only after the broker confirmed it (the replay round runs every 60 s)."""
    observer, cfg, _ = online
    marker_ts = 1_600_000_000_000
    name = f"{marker_ts}_000000.txt"
    mark = gateway.b.mark()
    gateway.b.put(json.dumps({"ts": marker_ts, "values": {"HIL_REPLAY": 1}}), f"/var/lib/gateway/storage/{name}")
    msg = observer.wait_json(TELEMETRY, lambda d: d.get("ts") == marker_ts, timeout=150)
    assert msg["values"] == {"HIL_REPLAY": 1}
    gateway.b.wait_log(mark, rf"Replayed and deleted \d+ stored payload", timeout=30)
    assert not gateway.b.exists(f"/var/lib/gateway/storage/{name}")
