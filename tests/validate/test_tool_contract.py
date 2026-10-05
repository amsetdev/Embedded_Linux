"""The desktop tool's output must be read correctly by the firmware.

Builds configs with the tool's own DeviceConfig (tools/smart_rtu_tool/core/
register_model.py, the code behind "Send to device"), then runs them through
every rule and the real firmware parser. Known contract bugs are strict xfails:
they turn red once fixed, as a reminder to make them normal tests.
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


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: the tool doesn't write wifi.enable, so the firmware's json_get_int() runs on into "
    "modbus_tcp and reads its 'enable': Wi-Fi is disabled whenever Modbus TCP is (main.c, wifi.c)"))
def test_wifi_stays_enabled_when_modbus_tcp_disabled():
    view = firmware_view(tool_config(modbus_tcp_enable=0, modbus_tcp_ip="").to_full_config_json())
    assert view["fields"]["wifi.enable"]["value"] == 1


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: for a register using the device's slave (slave_id 0 in the tool) the tool omits "
    "slave_id, and the firmware's parse_registers() takes the NEXT register's slave_id"))
def test_register_with_device_default_slave_polled_from_device_slave():
    points = [RegisterPoint("A", 0, slave_id=0), RegisterPoint("B", 1, slave_id=2)]
    view = firmware_view(tool_config(points=points, slave_id=1).to_full_config_json())
    assert [r["slave_id"] for r in view["registers"]] == [1, 2]


@pytest.mark.xfail(strict=True, reason=(
    "Known issue: settings_defaults() formats the OTA topics with the default device_id AMSET-001 "
    "before the file's device_id is read, and the tool writes no 'ota' section: every gateway "
    "listens on devices/AMSET-001/ota/*"))
def test_ota_topics_follow_configured_device_id():
    view = firmware_view(tool_config(device_id="GW-42").to_full_config_json())
    assert view["fields"]["ota.app_topic"]["value"] == "devices/GW-42/ota/app"


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
@pytest.mark.xfail(strict=True, reason=(
    "Known issue: DeviceConfig.validate_all() only checks ranges and exact duplicates; it accepts "
    "labels the firmware truncates or misparses and overlapping 32-bit registers"))
def test_tool_rejects_what_firmware_mishandles(point):
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
