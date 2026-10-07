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
    RegisterPoint("A", 0, register_type="discrete", data_type="uint16"),
    RegisterPoint("A", 65534, data_type="int32"),
])
def test_point_validation_accepts(point):
    assert point.validate() is None


def test_validate_all_reports_every_bad_row_and_duplicates():
    config = DeviceConfig(points=[
        RegisterPoint("A", 1), RegisterPoint("", 2), RegisterPoint("C", 1), RegisterPoint("D", 1, slave_id=2),
    ])
    errors = config.validate_all()
    assert any(e.startswith("Row 2") and "Label" in e for e in errors)
    assert any(e.startswith("Row 3:") and "overlaps register 1" in e and "row 1" in e for e in errors)
    assert not any(e.startswith("Row 4") for e in errors), "same address on another slave is fine"


@pytest.mark.parametrize("label, error", [
    ("L" * 64, "64 bytes"),
    ("é" * 32, "64 bytes"),
    ('TEMP"C', 'must not contain'),
    ("A\\B", "must not contain"),
])
def test_label_the_firmware_would_cut_or_misparse_rejected(label, error):
    """Regression: labels the firmware truncates (char[64]) or that break the telemetry
    JSON were accepted."""
    assert error in RegisterPoint(label, 0).validate()


def test_label_of_63_bytes_accepted():
    assert RegisterPoint("L" * 63, 0).validate() is None


def test_overlapping_32bit_registers_rejected():
    """Regression: only exact duplicates were found, not an int32/float32 overlapping
    the second register of another."""
    config = DeviceConfig(slave_id=1, points=[
        RegisterPoint("F", 0, data_type="float32"),
        RegisterPoint("H", 1, slave_id=1, data_type="int32"),      # overlaps F's second register
        RegisterPoint("N", 3, data_type="float32"),                # right after H (1-2): fine
        RegisterPoint("X", 1, slave_id=2, data_type="float32"),    # other slave: fine
        RegisterPoint("C", 1, register_type="coil"),               # other table: fine
    ])
    errors = config.validate_all()
    assert len(errors) == 1 and errors[0].startswith("Row 2:") and "overlaps register 1" in errors[0]


def test_default_slave_counts_as_device_slave_for_overlaps():
    config = DeviceConfig(slave_id=3, points=[RegisterPoint("A", 5), RegisterPoint("B", 5, slave_id=3)])
    assert any("overlaps" in e for e in config.validate_all())


@pytest.mark.parametrize("point, error", [
    (RegisterPoint("C", 0, register_type="coil", data_type="float32"), "needs holding/input"),
    (RegisterPoint("I", 65535, data_type="int32"), "max 65535"),
])
def test_32bit_type_limits(point, error):
    assert error in point.validate()


def test_duplicate_labels_rejected():
    config = DeviceConfig(points=[RegisterPoint("A", 0), RegisterPoint("A", 1)])
    assert any("duplicate label 'A'" in e for e in config.validate_all())


def test_full_config_has_every_section_the_firmware_reads():
    d = DeviceConfig(points=[RegisterPoint("A", 0)]).to_full_config_dict()
    assert list(d) == ["device", "wifi", "modbus_tcp", "mqtt", "registers"]
    assert set(d["device"]) == {"device_id", "slave_id", "baud", "parity", "stop_bits", "interval_sec"}
    assert set(d["mqtt"]) == {"broker", "port", "client_id", "ca_cert", "device_cert", "private_key", "topic"}
    assert set(d["wifi"]) == {"ssid", "password", "country", "enable"}
    assert d["registers"] == [{"label": "A", "address": 0, "type": "holding", "data_type": "uint16", "slave_id": 1}]


def test_register_slave_id_always_written():
    """Regression: slave_id was omitted for "device default" (0) registers, and the
    firmware then took the next register's slave_id."""
    regs = DeviceConfig(slave_id=5, points=[RegisterPoint("A", 0), RegisterPoint("B", 1, slave_id=3)]
                        ).to_full_config_dict()["registers"]
    assert [r["slave_id"] for r in regs] == [5, 3]


def test_wifi_enable_and_country_written():
    """Regression: wifi.enable was not written, so the firmware read modbus_tcp's 'enable'."""
    wifi = DeviceConfig(wifi_enable=0, wifi_country="DE").to_full_config_dict()["wifi"]
    assert (wifi["enable"], wifi["country"]) == (0, "DE")
    assert DeviceConfig().to_full_config_dict()["wifi"]["enable"] == 1


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
        wifi_ssid="plant", wifi_password="pw", wifi_enable=0, wifi_country="DE", modbus_tcp_enable=1, modbus_tcp_ip="10.0.0.2",
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
