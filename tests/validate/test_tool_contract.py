"""The desktop tool's output must be read correctly by the firmware.

Builds configs with the tool's own DeviceConfig (tools/smart_rtu_tool/core/
register_model.py, the code behind "Send to device"), then runs them through
every rule and the real firmware parser. Tests starting with "Regression:" guard
contract bugs that were fixed (DOCS/TESTS_GUIDE.md §7).
"""

import csv
import io
import json

import pytest

from config_checks import check_config
from firmware_view import firmware_view
from core.register_model import DeviceConfig, RegisterPoint

pytestmark = pytest.mark.validate


def tool_config(**overrides):
    """A complete config as the tool builds it, every register with an explicit slave."""
    points = [
        RegisterPoint("FLOW", 0, slave_id=1, register_type="holding", data_type="float32"),
        RegisterPoint("TOTAL", 2, slave_id=1, register_type="input", data_type="int32"),
        RegisterPoint("PUMP_ON", 0, slave_id=2, register_type="coil", data_type="uint16"),
        RegisterPoint("LEVEL", 10, slave_id=2, register_type="input", data_type="uint16"),
    ]
    fields = dict(device_id="GW-42", slave_id=1, baud=19200, interval_sec=10, parity="Even",
                  stop_bits=1, points=points, wifi_ssid="plant", wifi_password="secret-pass",
                  modbus_tcp_enable=1, modbus_tcp_ip="10.0.0.5", modbus_tcp_port=502,
                  mqtt_broker="example-ats.iot.ap-south-1.amazonaws.com", mqtt_client_id="GW-42",
                  mqtt_topic="devices/GW-42/telemetry")
    fields.update(overrides)
    return DeviceConfig(**fields)


def problems_of(config):
    text = config.to_full_config_json()
    return check_config(json.loads(text), text)


def test_tool_config_read_correctly_by_firmware():
    problems = problems_of(tool_config())
    assert not problems, "\n".join(problems)


def test_tool_writes_every_register_type_and_data_type():
    points = [RegisterPoint(f"{t}_{d}", 10 * i + 2 * j, slave_id=1, register_type=t, data_type=d)
              for i, t in enumerate(["holding", "input"]) for j, d in enumerate(["uint16", "int32", "float32"])]
    points += [RegisterPoint(f"{t}_bit", 0, slave_id=1, register_type=t) for t in ["coil", "discrete"]]
    problems = problems_of(tool_config(points=points))
    assert not problems, "\n".join(problems)


def test_wifi_stays_enabled_when_modbus_tcp_disabled():
    """Regression: the tool didn't write wifi.enable and the firmware read modbus_tcp's
    'enable' instead: Wi-Fi was disabled whenever Modbus TCP was."""
    config = tool_config(modbus_tcp_enable=0, modbus_tcp_ip="")
    assert config.to_full_config_dict()["wifi"]["enable"] == 1
    view = firmware_view(config.to_full_config_json())
    assert view["fields"]["wifi.enable"]["value"] == 1
    assert not problems_of(config)


def test_wifi_can_be_disabled():
    view = firmware_view(tool_config(wifi_enable=0).to_full_config_json())
    assert view["fields"]["wifi.enable"]["value"] == 0


def test_register_with_device_default_slave_polled_from_device_slave():
    """Regression: for slave_id 0 ("device default") the tool omitted slave_id and the
    firmware took the NEXT register's slave_id."""
    points = [RegisterPoint("A", 0, slave_id=0), RegisterPoint("B", 1, slave_id=2)]
    config = tool_config(points=points, slave_id=1)
    assert [r["slave_id"] for r in config.to_full_config_dict()["registers"]] == [1, 2]
    view = firmware_view(config.to_full_config_json())
    assert [r["slave_id"] for r in view["registers"]] == [1, 2]


def test_firmware_uses_device_slave_when_register_slave_missing():
    """Regression (firmware side): parse_registers() reads each register object only."""
    text = json.dumps({"device": {"slave_id": 4}, "registers": [
        {"label": "A", "address": 0, "type": "holding", "data_type": "uint16"},
        {"label": "B", "address": 1, "type": "holding", "data_type": "uint16", "slave_id": 9}]})
    assert [r["slave_id"] for r in firmware_view(text)["registers"]] == [4, 9]


def test_ota_topics_follow_configured_device_id():
    """Regression: OTA topics were formatted with the default device_id AMSET-001 before
    the file's device_id was read: every gateway listened on devices/AMSET-001/ota/*."""
    view = firmware_view(tool_config(device_id="GW-42").to_full_config_json())
    assert view["fields"]["ota.app_topic"]["value"] == "devices/GW-42/ota/app"
    assert view["fields"]["ota.system_topic"]["value"] == "devices/GW-42/ota/system"
    assert view["fields"]["ota.status_topic"]["value"] == "devices/GW-42/ota/status"


def test_ota_section_still_overrides_topics():
    d = tool_config(device_id="GW-42").to_full_config_dict()
    d["ota"] = {"enable": 1, "app_topic": "custom/app", "system_topic": "custom/sys",
                "status_topic": "custom/status", "download_dir": "/tmp/ota", "app_version": "1.0.0",
                "app_binary_path": "/home/root/edb_c/linking/main"}
    view = firmware_view(json.dumps(d))
    assert view["fields"]["ota.app_topic"]["value"] == "custom/app"


def test_escaped_strings_read_like_json():
    """Regression: the firmware didn't decode JSON escapes (a value stopped at \\")."""
    d = tool_config().to_full_config_dict()
    d["wifi"]["password"] = 'pa"ss\\word/x'
    view = firmware_view(json.dumps(d))
    assert view["fields"]["wifi.password"]["value"] == 'pa"ss\\word/x'


TOOL_GAPS = [
    pytest.param(RegisterPoint("L" * 64, 50, slave_id=1), id="label_64_bytes"),
    pytest.param(RegisterPoint('TEMP"C', 50, slave_id=1), id="label_with_quote"),
    pytest.param(RegisterPoint("HALF", 1, slave_id=1, data_type="int32"), id="overlaps_float_at_0"),
]


@pytest.mark.parametrize("point", TOOL_GAPS)
def test_validate_rules_reject_tool_gap_cases(point):
    config = tool_config()
    config.points.append(point)
    assert problems_of(config)


@pytest.mark.parametrize("point", TOOL_GAPS)
def test_tool_rejects_what_firmware_mishandles(point):
    """Regression: DeviceConfig.validate_all() only checked ranges and exact duplicates;
    it accepted labels the firmware cuts or misparses and overlapping 32-bit registers."""
    config = tool_config()
    config.points.append(point)
    assert config.validate_all(), "the tool's own validation accepted it"


def test_csv_export_matches_json():
    """smart_rtu_config.csv (sent next to the JSON) carries the same content."""
    config = tool_config(points=tool_config().points + [RegisterPoint("DEF", 20)])
    d = config.to_full_config_dict()
    sections, current = {}, None
    for row in csv.reader(io.StringIO(config.to_full_config_csv_string())):
        if not row:
            continue
        if row[0].startswith("[") and row[0].endswith("]"):
            current = sections.setdefault(row[0][1:-1].lower(), [])
            continue
        current.append(row)
    for name in ["device", "wifi", "modbus_tcp", "mqtt"]:
        assert {k: v for k, v in sections[name]} == {k: str(v) for k, v in d[name].items()}, name
    header, *rows = sections["registers"]
    assert header == ["slave_id", "label", "address", "type", "data_type"]
    assert rows == [[str(r.get("slave_id", 0)), r["label"], str(r["address"]), r["type"], r["data_type"]]
                    for r in d["registers"]]


def test_firmware_subscribes_to_tool_config_topic():
    """Regression: the tool published configs on amset/<id>/config/set but the firmware
    never subscribed to it (MQTT 'Push' always timed out)."""
    import re
    from conftest import REPO
    header = (REPO / "inc/config_push.h").read_text()
    set_fmt = re.search(r'#define\s+CONFIG_SET_TOPIC_FMT\s+"([^"]+)"', header).group(1)
    ack_fmt = re.search(r'#define\s+CONFIG_ACK_TOPIC_FMT\s+"([^"]+)"', header).group(1)
    assert set_fmt % "GW-1" == "amset/GW-1/config/set"
    assert ack_fmt % "GW-1" == "amset/GW-1/config/ack"
    mqtt_c = (REPO / "src/cloud/mqtt.c").read_text()
    assert re.search(r"snprintf\(config_topic,[^;]*CONFIG_SET_TOPIC_FMT", mqtt_c)
    assert re.search(r"mosquitto_subscribe\(m, NULL, config_topic, 1\)", mqtt_c)
    tool = (REPO / "tools/smart_rtu_tool/core/mqtt_transport.py").read_text()
    assert 'f"amset/{device_id}/config/set"' in tool and 'f"amset/{device_id}/config/ack"' in tool
