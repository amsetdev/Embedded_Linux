"""The rules must reject what the firmware would mishandle, and allow what it handles.

Each negative case is a mutation of the known-good fixture plus a text the
error must contain. A case that stops failing means a rule was weakened.
"""

import copy
import json

import pytest

from conftest import REPO
from config_checks import check_config

pytestmark = pytest.mark.validate

GOOD = json.loads((REPO / "tests/fixtures/configs/full_config.json").read_text())


def mutate(fn):
    cfg = copy.deepcopy(GOOD)
    fn(cfg)
    return cfg


def set_(path, value):
    """Mutation that sets cfg[a][b]... = value."""
    def fn(cfg):
        *parents, last = path
        node = cfg
        for p in parents:
            node = node[p]
        node[last] = value
    return fn


def delete(path):
    def fn(cfg):
        *parents, last = path
        node = cfg
        for p in parents:
            node = node[p]
        del node[last]
    return fn


def reg(**fields):
    base = {"label": "EXTRA", "address": 900, "type": "holding", "data_type": "uint16", "slave_id": 1}
    return {**base, **fields}


def add_regs(*regs):
    return lambda cfg: cfg["registers"].extend(regs)


def many_regs(n, label_len=8):
    return lambda cfg: cfg.__setitem__("registers", [
        reg(label=f"R{i:0{label_len - 1}d}", address=i % 65536, slave_id=1 + i // 65536) for i in range(n)])


NEGATIVE = {
    # --- the firmware's parser would read something else -------------------
    "label_with_quote": (set_(["registers", 0, "label"], 'TEMP"C'), "label"),
    "label_with_backslash": (set_(["registers", 0, "label"], "A\\B"), "label"),
    "ssid_with_quote": (set_(["wifi", "ssid"], 'my"net'), "wifi/ssid"),
    "label_64_bytes": (set_(["registers", 0, "label"], "L" * 64), "the firmware keeps 63"),
    "label_utf8_64_bytes": (set_(["registers", 0, "label"], "é" * 32), "64 bytes"),
    "device_id_64_chars": (set_(["device", "device_id"], "D" * 64), "device.device_id"),
    "broker_256_chars": (set_(["mqtt", "broker"], "b" * 256), "mqtt.broker"),
    "baud_as_string": (set_(["device", "baud"], "9600"), "device.baud"),
    "enable_as_bool": (set_(["modbus_tcp", "enable"], True), "modbus_tcp.enable"),
    "interval_as_float": (set_(["device", "interval_sec"], 2.5), "device.interval_sec"),
    # --- schema ------------------------------------------------------------
    "missing_device_section": (delete(["device"]), "'device' is a required property"),
    "missing_registers": (delete(["registers"]), "'registers' is a required property"),
    "unknown_section": (set_(["modbus_rtu"], {}), "Additional properties"),
    "unknown_register_type": (set_(["registers", 0, "type"], "holding_register"), "holding_register"),
    "unknown_data_type": (set_(["registers", 0, "data_type"], "uint32"), "uint32"),
    "parity_letter": (set_(["device", "parity"], "N"), "device/parity"),
    "stop_bits_3": (set_(["device", "stop_bits"], 3), "device/stop_bits"),
    "baud_non_standard": (set_(["device", "baud"], 10000), "device/baud"),
    "device_slave_id_0": (set_(["device", "slave_id"], 0), "device/slave_id"),
    "register_slave_id_248": (set_(["registers", 0, "slave_id"], 248), "registers/0/slave_id"),
    "interval_0": (set_(["device", "interval_sec"], 0), "device/interval_sec"),
    "address_65536": (set_(["registers", 0, "address"], 65536), "registers/0/address"),
    "mqtt_port_0": (set_(["mqtt", "port"], 0), "mqtt/port"),
    "relative_cert_path": (set_(["mqtt", "ca_cert"], "ca.crt"), "mqtt/ca_cert"),
    "empty_label": (set_(["registers", 0, "label"], ""), "registers/0/label"),
    "register_unit_not_sent": (set_(["registers", 0, "unit"], "kWh"), "Additional properties"),
    "too_many_registers_2001": (many_regs(2001), "too many registers"),
    # --- rules ---------------------------------------------------------------
    "duplicate_label": (add_regs(reg(label="S1_HOLDING_UINT16")), "duplicate label"),
    "overlap_same_register": (add_regs(reg(address=100)), "overlap"),
    "overlap_float_second_half": (add_regs(reg(address=103)), "overlap"),
    "overlap_int32_starting_inside_float": (add_regs(reg(address=105, data_type="int32")), "overlap"),
    "int32_at_65535": (add_regs(reg(address=65535, data_type="int32")), "beyond 65535"),
    "float_on_coil": (add_regs(reg(type="coil", data_type="float32")), "32-bit types need holding/input"),
    "tcp_enabled_bad_ip": (set_(["modbus_tcp", "ip"], "192.168.1"), "not an IPv4 address"),
    "topic_wildcard_hash": (set_(["mqtt", "topic"], "devices/#"), "wildcard"),
    "topic_wildcard_plus": (set_(["mqtt", "topic"], "devices/+/t"), "wildcard"),
    "topic_8_slashes": (set_(["mqtt", "topic"], "a/b/c/d/e/f/g/h/i"), "AWS IoT allows 7"),
    "topic_empty_with_broker": (set_(["mqtt", "topic"], ""), "mqtt.topic is empty"),
    "telemetry_too_large": (many_regs(2000, label_len=60), "telemetry too large"),
}


@pytest.mark.parametrize("name", NEGATIVE)
def test_rejected(name):
    mutation, expected = NEGATIVE[name]
    problems = check_config(mutate(mutation))
    assert problems, f"{name}: config was accepted"
    assert any(expected in p for p in problems), f"{name}: no problem mentions {expected!r}:\n  " + "\n  ".join(problems)


ALLOWED = {
    "same_address_different_slave": add_regs(reg(address=100, slave_id=3)),
    "same_address_different_table": add_regs(reg(address=900), reg(address=900, type="coil", label="C900")),
    "float_right_after_float": add_regs(reg(address=106, data_type="float32", label="NEXT")),
    "label_63_bytes": set_(["registers", 0, "label"], "L" * 63),
    "exactly_2000_registers": many_regs(2000),
    "without_optional_sections": lambda c: (c.pop("ota"), c.pop("watchdog")),
    "wifi_without_country": delete(["wifi", "country"]),
    "tcp_disabled_empty_ip": lambda c: c["modbus_tcp"].update(enable=0, ip=""),
    "no_registers": set_(["registers"], []),
    "register_without_slave_id_last": lambda c: (c["registers"][-1].pop("slave_id"),
                                                 c["registers"][-1].__setitem__("address", 500)),
    # Regression: the firmware's parser read a key missing from one section from a LATER
    # section (wifi.enable from modbus_tcp: Wi-Fi off whenever Modbus TCP was off).
    "wifi_without_enable_keeps_default": lambda c: (c["wifi"].pop("enable"),
                                                    c["modbus_tcp"].update(enable=0, ip="")),
    # Regression: a register without slave_id took the NEXT register's slave_id.
    "register_without_slave_id_uses_device_slave": lambda c: (
        c["registers"][0].pop("slave_id"), c["registers"].insert(1, reg(slave_id=2))),
}


@pytest.mark.parametrize("name", ALLOWED)
def test_allowed(name):
    problems = check_config(mutate(ALLOWED[name]))
    assert not problems, f"{name}: rejected:\n  " + "\n  ".join(problems)
