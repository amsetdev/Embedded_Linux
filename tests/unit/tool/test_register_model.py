"""core/register_model.py: RegisterPoint / DeviceConfig validation, the combined
config file (JSON + CSV) and the project file round trip.

How the firmware reads the JSON is checked in tests/validate/test_tool_contract.py.
"""

import json

import pytest

from core.register_model import DeviceConfig, RegisterPoint

pytestmark = pytest.mark.unit


@pytest.mark.parametrize("point, error", [
    (RegisterPoint("", 0), "Label cannot be empty"),
    (RegisterPoint("   ", 0), "Label cannot be empty"),
    (RegisterPoint("A", -1), "out of range"),
    (RegisterPoint("A", 65536), "out of range"),
    (RegisterPoint("A", 0, slave_id=248), "Slave ID 248"),
    (RegisterPoint("A", 0, register_type="holding_register"), "Invalid register_type"),
    (RegisterPoint("A", 0, data_type="uint32"), "Invalid data_type"),
])
def test_point_validation_rejects(point, error):
    assert error in point.validate()


@pytest.mark.parametrize("point", [
    RegisterPoint("A", 0), RegisterPoint("A", 65535), RegisterPoint("A", 0, slave_id=247),
    RegisterPoint("A", 0, register_type="discrete", data_type="float32"),
])
def test_point_validation_accepts(point):
    assert point.validate() is None


def test_validate_all_reports_every_bad_row_and_duplicates():
    config = DeviceConfig(points=[
        RegisterPoint("A", 1), RegisterPoint("", 2), RegisterPoint("C", 1), RegisterPoint("D", 1, slave_id=2),
    ])
    errors = config.validate_all()
    assert any(e.startswith("Row 2") and "Label" in e for e in errors)
    assert any("Row 3: duplicate address 1" in e and "row 1" in e for e in errors)
    assert not any(e.startswith("Row 4") for e in errors), "same address on another slave is fine"


def test_full_config_has_every_section_the_firmware_reads():
    d = DeviceConfig(points=[RegisterPoint("A", 0)]).to_full_config_dict()
    assert list(d) == ["device", "wifi", "modbus_tcp", "mqtt", "registers"]
    assert set(d["device"]) == {"device_id", "slave_id", "baud", "parity", "stop_bits", "interval_sec"}
    assert set(d["mqtt"]) == {"broker", "port", "client_id", "ca_cert", "device_cert", "private_key", "topic"}
    assert d["registers"] == [{"label": "A", "address": 0, "type": "holding", "data_type": "uint16"}]


def test_register_slave_id_written_only_when_set():
    regs = DeviceConfig(points=[RegisterPoint("A", 0), RegisterPoint("B", 1, slave_id=3)]).to_full_config_dict()["registers"]
    assert "slave_id" not in regs[0]      # 0 = device default (see validate known issue)
    assert regs[1]["slave_id"] == 3


def test_full_config_json_is_the_dict():
    config = DeviceConfig(device_id="GW-1", points=[RegisterPoint("A", 0, data_type="float32")])
    assert json.loads(config.to_full_config_json()) == config.to_full_config_dict()


def test_save_full_config_writes_json_and_csv(tmp_path):
    config = DeviceConfig(points=[RegisterPoint("A", 0)])
    config.save_full_config_json(tmp_path / "c.json")
    config.save_full_config_csv(tmp_path / "c.csv")
    assert json.loads((tmp_path / "c.json").read_text()) == config.to_full_config_dict()
    with open(tmp_path / "c.csv", newline="") as f:
        assert f.read() == config.to_full_config_csv_string()


def test_project_file_round_trip_keeps_every_field(tmp_path):
    original = DeviceConfig(
        device_id="GW-7", slave_id=5, baud=19200, interval_sec=12, parity="Odd", stop_bits=2,
        wifi_ssid="plant", wifi_password="pw", modbus_tcp_enable=1, modbus_tcp_ip="10.0.0.2",
        modbus_tcp_port=1502, modbus_tcp_slave_id=9, mqtt_broker="b", mqtt_port=8884,
        mqtt_client_id="cid", mqtt_ca_cert="/a", mqtt_device_cert="/b", mqtt_private_key="/c",
        mqtt_topic="t/x",
        points=[RegisterPoint("A", 1, slave_id=2, register_type="input", data_type="int32", unit="kWh")])
    path = tmp_path / "project.json"
    original.save_json(path)
    assert DeviceConfig.load_json(path) == original


def test_project_file_from_older_version_loads_with_defaults(tmp_path):
    path = tmp_path / "old.json"
    path.write_text(json.dumps({"device_id": "OLD", "points": [{"label": "A", "address": 3}],
                                "removed_field": 1}))
    config = DeviceConfig.load_json(path)
    assert config.device_id == "OLD" and config.baud == 9600
    assert config.points == [RegisterPoint("A", 3)]


def test_esp32_nvs_export_import(tmp_path):
    path = tmp_path / "esp.json"
    path.write_text(json.dumps({"device_id": "ESP", "slave_id": 4,
                                "data": [{"Label": "P1", "Address": 10}, {"Label": "P2", "Address": 12}]}))
    config = DeviceConfig.load_esp32_nvs_json(path)
    assert (config.device_id, config.slave_id) == ("ESP", 4)
    assert [(p.label, p.address, p.register_type) for p in config.points] == [("P1", 10, "holding"), ("P2", 12, "holding")]
