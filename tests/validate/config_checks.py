"""Rules for smart_rtu_config.json that a JSON schema can't express.

Each check returns a list of problems (strings), so one run reports everything
wrong with a config. check_config() runs them all, including the schema.

The most important check is check_firmware_reads(): it runs the REAL firmware
parser (firmware_view.py) on the file and compares every value the firmware
ends up with against what the file says. The firmware's parser
(src/util/json.c) searches each key with strstr() from the start of its
section to the END of the file, so a key missing from one section is silently
read from a later one, a string stops at the first '"', and long strings are
cut to the buffer size. Comparing against the real parser catches all of that,
including cases nobody has thought of yet.
"""

import ipaddress
import json
import re
from collections import Counter
from pathlib import Path

from jsonschema import Draft202012Validator

from firmware_view import firmware_defaults, firmware_view

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "tests" / "schemas" / "smart_rtu_config.schema.json"
MQTT_H = REPO / "inc" / "mqtt.h"

# Config value -> what the firmware should store (data.c: reg_type_from_string,
# data_type_from_string). Checked against the real parser in test_type_consistency.py.
REG_TYPE_CODE = {"holding": 0, "input": 1, "coil": 2, "discrete": 3}   # RegType, inc/data.h
DATA_TYPE_CODE = {"uint16": "w", "int32": "d", "float32": "f"}
REGISTER_WIDTH = {"uint16": 1, "int32": 2, "float32": 2}               # Modbus registers used
BIT_TABLES = {"coil", "discrete"}

# Telemetry (build_payload() in src/cloud/mqtt.c): {"ts":<ms>,"values":{"<label>":<value>,...}}
# into a PAYLOAD_MAX buffer; it stops adding values 128 bytes before the end.
PAYLOAD_HEADER = len('{"ts":1767225600000,"values":{') + len("}}")
VALUE_WIDTH = {"w": len("-2147483648"), "d": len("-2147483648"),
               "f": len("-340282346638528859811704183484516925440.00"), "b": len("false")}
PAYLOAD_RESERVE = 128

AWS_TOPIC_MAX_BYTES = 256
AWS_TOPIC_MAX_SLASHES = 7

# Hints for differences between file and firmware with a known cause.
LEAK_HINT = ("the key is missing here, so the firmware's parser (src/util/json.c) took it from "
             "a later part of the file; write it explicitly")


def payload_max():
    """PAYLOAD_MAX from inc/mqtt.h."""
    m = re.search(r"#define\s+PAYLOAD_MAX\s+(\d+)", MQTT_H.read_text())
    if not m:
        raise RuntimeError("PAYLOAD_MAX not found in inc/mqtt.h")
    return int(m.group(1))


def _schema_validator():
    schema = json.loads(SCHEMA_PATH.read_text())
    return Draft202012Validator(schema)


def check_schema(cfg):
    """JSON schema (tests/schemas/smart_rtu_config.schema.json)."""
    problems = []
    for err in sorted(_schema_validator().iter_errors(cfg), key=lambda e: list(e.absolute_path)):
        where = "/".join(str(p) for p in err.absolute_path) or "(root)"
        problems.append(f"schema: {where}: {err.message}")
    return problems


# Defaults built from the device ID (settings.c: OTA_*_TOPIC_DEF "devices/%s/ota/...").
DEVICE_ID_DEFAULTS = ("ota.app_topic", "ota.system_topic", "ota.status_topic")


def _expected_fields(cfg):
    """What every firmware field should be: the file's value, or the default when absent."""
    expected = {}
    defaults = firmware_defaults()["fields"]
    default_id = defaults["device.device_id"]["value"]
    device = cfg.get("device") if isinstance(cfg.get("device"), dict) else {}
    device_id = device.get("device_id", default_id)
    for path, default in defaults.items():
        section, key = path.split(".", 1)
        sec = cfg.get(section)
        if isinstance(sec, dict) and key in sec:
            expected[path] = sec[key]
        elif path in DEVICE_ID_DEFAULTS and isinstance(device_id, str):
            expected[path] = default["value"].replace(f"/{default_id}/", f"/{device_id}/")
        else:
            expected[path] = default["value"]
    return expected


def check_firmware_reads(cfg, text):
    """Run the real firmware parser on the file; every value must match the file."""
    problems = []
    view = firmware_view(text)
    if view["settings_load"] != 0:
        problems.append("firmware: settings_load() could not read the file")

    for path, want in _expected_fields(cfg).items():
        got = view["fields"][path]
        section, key = path.split(".", 1)
        present = isinstance(cfg.get(section), dict) and key in cfg[section]
        if "size" in got:   # string field
            if isinstance(want, str) and len(want.encode()) > got["size"] - 1:
                problems.append(f"too long: {path} is {len(want.encode())} bytes, "
                                f"the firmware keeps {got['size'] - 1} (char[{got['size']}])")
                continue
        if got["value"] != want:
            hint = "" if present else f" ({LEAK_HINT})"
            problems.append(f"firmware reads {path} = {got['value']!r}, the file means {want!r}{hint}")

    regs = cfg.get("registers")
    if isinstance(regs, list):
        problems += _check_firmware_registers(cfg, regs, view)
    return problems


def _check_firmware_registers(cfg, regs, view):
    problems = []
    fw_regs = view["registers"]
    limit = view["max_points"]
    if len(regs) > limit:
        problems.append(f"too many registers: {len(regs)}, the firmware keeps the first {limit} (MAX_POINTS)")
    expected_count = min(len(regs), limit)
    if len(fw_regs) != expected_count:
        problems.append(f"firmware parsed {len(fw_regs)} registers, the file has {expected_count}")
    default_slave = view["fields"]["device.slave_id"]["value"]

    for i, (r, fw) in enumerate(zip(regs, fw_regs)):
        if not isinstance(r, dict):
            continue
        name = f"registers[{i}] {r.get('label', '?')!r}"
        label = r.get("label", "")
        if isinstance(label, str) and len(label.encode()) > view["label_size"] - 1:
            problems.append(f"too long: {name} label is {len(label.encode())} bytes, "
                            f"the firmware keeps {view['label_size'] - 1}")
        elif fw["label"] != label:
            problems.append(f"firmware reads {name} label as {fw['label']!r}")
        want = {
            "address": r.get("address"),
            "reg_type": REG_TYPE_CODE.get(r.get("type")),
            "data_type": DATA_TYPE_CODE.get(r.get("data_type")),
            "slave_id": r.get("slave_id", default_slave),
        }
        for key, value in want.items():
            if value is not None and fw[key] != value:
                field = {"reg_type": "type", "data_type": "data_type"}.get(key, key)
                hint = f" ({LEAK_HINT})" if field not in r else ""
                problems.append(f"firmware reads {name} {key} = {fw[key]!r}, the file means {value!r}{hint}")
    return problems


def check_registers(cfg):
    """Labels, register ranges, overlaps and telemetry size."""
    regs = cfg.get("registers")
    if not isinstance(regs, list):
        return []
    problems = []
    device = cfg.get("device") if isinstance(cfg.get("device"), dict) else {}
    default_slave = device.get("slave_id", 1)

    labels = Counter(r.get("label") for r in regs if isinstance(r, dict))
    for label, n in labels.items():
        if n > 1:
            problems.append(f"duplicate label {label!r} ({n} times): labels are the telemetry JSON keys")

    used = {}   # (slave, table, register) -> description of the first user
    for i, r in enumerate(regs):
        if not isinstance(r, dict) or not isinstance(r.get("address"), int):
            continue
        table, dtype = r.get("type"), r.get("data_type")
        width = REGISTER_WIDTH.get(dtype, 1)
        name = f"registers[{i}] {r.get('label', '?')!r}"
        if table in BIT_TABLES and width > 1:
            problems.append(f"{name}: {dtype} on a {table} table; 32-bit types need holding/input registers")
        last = r["address"] + width - 1
        if last > 65535:
            problems.append(f"{name}: {dtype} at {r['address']} needs register {last}, beyond 65535")
        slave = r.get("slave_id", default_slave)
        for reg in range(r["address"], last + 1):
            k = (slave, table, reg)
            if k in used:
                problems.append(f"overlap: {name} ({dtype}, {table} {r['address']}"
                                f"{'-' + str(last) if last != r['address'] else ''}) reuses slave {slave} "
                                f"{table} register {reg} of {used[k]}")
                break
        for reg in range(r["address"], last + 1):
            used.setdefault((slave, table, reg), f"{name} ({dtype})")

    worst = PAYLOAD_HEADER + sum(
        len(str(r.get("label", "")).encode()) + 3 + 1
        + VALUE_WIDTH[DATA_TYPE_CODE.get(r.get("data_type"), "w")]
        for r in regs if isinstance(r, dict))
    room = payload_max() - PAYLOAD_RESERVE
    if worst > room:
        problems.append(f"telemetry too large: up to {worst} bytes with these labels, the firmware "
                        f"stops adding values at {room} (PAYLOAD_MAX {payload_max()} in inc/mqtt.h); "
                        f"use fewer registers or shorter labels")
    return problems


def check_network(cfg):
    """Modbus TCP address, MQTT topic, AWS IoT limits."""
    problems = []
    tcp = cfg.get("modbus_tcp")
    if isinstance(tcp, dict) and tcp.get("enable") == 1:
        try:
            ipaddress.IPv4Address(tcp.get("ip", ""))
        except ValueError:
            problems.append(f"modbus_tcp.ip {tcp.get('ip')!r} is not an IPv4 address (Modbus TCP is enabled)")

    mqtt = cfg.get("mqtt")
    if isinstance(mqtt, dict):
        topic = mqtt.get("topic", "")
        if isinstance(topic, str):
            if mqtt.get("broker") and not topic:
                problems.append("mqtt.topic is empty but a broker is set: telemetry would be published to ''")
            if any(c in topic for c in "#+"):
                problems.append(f"mqtt.topic {topic!r} contains a wildcard (# or +): not allowed when publishing")
            if topic.startswith("$"):
                problems.append(f"mqtt.topic {topic!r} starts with '$' (reserved topics)")
            if len(topic.encode()) > AWS_TOPIC_MAX_BYTES:
                problems.append(f"mqtt.topic is longer than AWS IoT's {AWS_TOPIC_MAX_BYTES} bytes")
            if topic.count("/") > AWS_TOPIC_MAX_SLASHES:
                problems.append(f"mqtt.topic {topic!r} has {topic.count('/')} '/', AWS IoT allows {AWS_TOPIC_MAX_SLASHES}")
    return problems


def check_config(cfg, text=None):
    """All checks. text: the file's exact bytes/str (defaults to json.dumps(cfg, indent=2))."""
    if not isinstance(cfg, dict):
        return ["schema: (root): the config must be a JSON object"]
    if text is None:
        text = json.dumps(cfg, indent=2)
    problems = check_schema(cfg)
    problems += check_registers(cfg)
    problems += check_network(cfg)
    problems += check_firmware_reads(cfg, text)
    return problems


def check_file(path):
    """Load and check one file; JSON syntax errors are reported as problems."""
    raw = Path(path).read_bytes()
    try:
        cfg = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        return [f"not valid JSON: {e}"]
    return check_config(cfg, raw)
