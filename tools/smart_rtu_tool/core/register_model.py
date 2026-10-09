"""
register_model.py
------------------
Defines the RegisterPoint data model and the DeviceConfig container that
holds everything the tool needs about one device: Wi-Fi, device/Modbus
settings, and the register list with per-register slave_id, type, and
data_type for multi-slave support.

Everything is delivered to the board as ONE combined file in both a
.csv and a .json version, written to /etc/gateway/ (DOCS/DEPLOYMENT.md) as:

    smart_rtu_config.csv
    smart_rtu_config.json
"""

from __future__ import annotations
import csv
import io
import json
from dataclasses import dataclass, field, asdict
from typing import List, Optional


VALID_REG_TYPES = {"holding", "input", "coil", "discrete"}
VALID_DATA_TYPES = {"uint16", "int32", "float32"}

# Firmware limits (inc/data.h): label is char[LABEL_MAX = 64] -> 63 UTF-8 bytes.
LABEL_MAX_BYTES = 63
# Modbus registers each data type occupies (32-bit types: Address and Address+1).
REGISTER_WIDTH = {"uint16": 1, "int32": 2, "float32": 2}
BIT_TABLES = {"coil", "discrete"}


@dataclass
class RegisterPoint:
    label: str
    address: int
    slave_id: int = 0                  # 0 = use device default slave_id
    register_type: str = "holding"     # holding | input | coil | discrete
    data_type: str = "uint16"          # uint16 | int32 | float32
    unit: str = ""

    def validate(self) -> Optional[str]:
        """Returns an error string if invalid, else None."""
        if not self.label or not self.label.strip():
            return "Label cannot be empty"
        if len(self.label.encode("utf-8")) > LABEL_MAX_BYTES:
            return (f"Label is {len(self.label.encode('utf-8'))} bytes; the firmware keeps "
                    f"{LABEL_MAX_BYTES} (it would be cut)")
        if '"' in self.label or "\\" in self.label:
            return 'Label must not contain " or \\ (labels are telemetry JSON keys)'
        if self.address < 0 or self.address > 65535:
            return f"Address {self.address} out of range (0-65535)"
        if self.slave_id < 0 or self.slave_id > 247:
            return f"Slave ID {self.slave_id} out of range (0-247)"
        if self.register_type not in VALID_REG_TYPES:
            return f"Invalid register_type '{self.register_type}'"
        if self.data_type not in VALID_DATA_TYPES:
            return f"Invalid data_type '{self.data_type}'"
        width = REGISTER_WIDTH[self.data_type]
        if self.register_type in BIT_TABLES and width > 1:
            return f"{self.data_type} needs holding/input registers, not {self.register_type}"
        if self.address + width - 1 > 65535:
            return f"{self.data_type} at {self.address} needs register {self.address + width - 1} (max 65535)"
        return None


@dataclass
class DeviceConfig:
    """Everything about one target device's configuration."""
    device_id: str = "AMSET-001"
    slave_id: int = 1
    baud: int = 9600
    interval_sec: int = 30
    parity: str = "None"
    stop_bits: int = 1
    points: List[RegisterPoint] = field(default_factory=list)

    # ---- Wi-Fi (board's own network, if applicable) -------------------
    wifi_ssid: str = ""
    wifi_password: str = ""
    wifi_enable: int = 1
    wifi_country: str = "IN"

    # ---- Modbus TCP (optional second protocol path) ------------------
    modbus_tcp_enable: int = 0
    modbus_tcp_ip: str = ""
    modbus_tcp_port: int = 502
    modbus_tcp_slave_id: int = 1

    # ---- Board MQTT (AWS IoT Core mTLS) ------------------------------
    mqtt_broker: str = ""
    mqtt_port: int = 8883
    mqtt_client_id: str = ""
    mqtt_ca_cert: str = "/etc/gateway/certs/ca.crt"
    mqtt_device_cert: str = "/etc/gateway/certs/client.crt"
    mqtt_private_key: str = "/etc/gateway/certs/private.key"
    mqtt_topic: str = ""

    # ---- Combined config file (Wi-Fi + Device + Registers,
    # ALL IN ONE FILE) ------------------------------------------------
    def to_full_config_dict(self) -> dict:
        regs = []
        for p in self.points:
            r = {
                "label": p.label,
                "address": p.address,
                "type": p.register_type,
                "data_type": p.data_type,
            }
            # Always explicit: slave_id 0 in the tool means "the device's slave".
            r["slave_id"] = p.slave_id if p.slave_id > 0 else self.slave_id
            regs.append(r)

        return {
            "device": {
                "device_id": self.device_id,
                "slave_id": self.slave_id,
                "baud": self.baud,
                "parity": self.parity,
                "stop_bits": self.stop_bits,
                "interval_sec": self.interval_sec,
            },
            "wifi": {
                "ssid": self.wifi_ssid,
                "password": self.wifi_password,
                "country": self.wifi_country,
                "enable": self.wifi_enable,
            },
            "modbus_tcp": {
                "enable": self.modbus_tcp_enable,
                "ip": self.modbus_tcp_ip,
                "port": self.modbus_tcp_port,
                "slave_id": self.modbus_tcp_slave_id,
            },
            "mqtt": {
                "broker": self.mqtt_broker,
                "port": self.mqtt_port,
                "client_id": self.mqtt_client_id,
                "ca_cert": self.mqtt_ca_cert,
                "device_cert": self.mqtt_device_cert,
                "private_key": self.mqtt_private_key,
                "topic": self.mqtt_topic,
            },
            "registers": regs,
        }

    # ---- The ONE combined settings+registers file sent to/read from the
    # device (used by Read/Send/Erase on the Device Config page, AND by
    # Push Data to Device on the Data Transmission page — same file,
    # same content, everywhere) --------------------------------------
    def to_full_config_json(self) -> str:
        return json.dumps(self.to_full_config_dict(), indent=2)

    def save_full_config_json(self, filepath: str):
        with open(filepath, "w") as f:
            f.write(self.to_full_config_json())

    def to_full_config_csv_string(self) -> str:
        """
        ONE CSV containing everything: Wi-Fi, Device Settings, and the
        register list with per-register type info. Sections are marked
        with [SECTION] header rows. This is the same file sent to the
        device.
        """
        d = self.to_full_config_dict()
        buf = io.StringIO()
        writer = csv.writer(buf)

        writer.writerow(["[DEVICE]"])
        for k, v in d["device"].items():
            writer.writerow([k, v])
        writer.writerow([])

        writer.writerow(["[WIFI]"])
        for k, v in d["wifi"].items():
            writer.writerow([k, v])
        writer.writerow([])

        writer.writerow(["[MODBUS_TCP]"])
        for k, v in d["modbus_tcp"].items():
            writer.writerow([k, v])
        writer.writerow([])

        writer.writerow(["[MQTT]"])
        for k, v in d["mqtt"].items():
            writer.writerow([k, v])
        writer.writerow([])

        writer.writerow(["[REGISTERS]"])
        writer.writerow(["slave_id", "label", "address", "type", "data_type"])
        for r in d["registers"]:
            writer.writerow([
                r.get("slave_id", 0),
                r["label"],
                r["address"],
                r["type"],
                r["data_type"],
            ])

        return buf.getvalue()

    def save_full_config_csv(self, filepath: str):
        with open(filepath, "w", newline="") as f:
            f.write(self.to_full_config_csv_string())

    # ---- JSON (tool "project file" format) -------------------------
    def to_json(self) -> str:
        d = asdict(self)
        return json.dumps(d, indent=2)

    def save_json(self, filepath: str):
        with open(filepath, "w") as f:
            f.write(self.to_json())

    @staticmethod
    def load_json(filepath: str) -> "DeviceConfig":
        with open(filepath, "r") as f:
            d = json.load(f)
        points = [RegisterPoint(**p) for p in d.get("points", [])]
        # Restore every scalar field the dataclass knows about (not just
        # the original four) so Wi-Fi and Modbus Map Sizing round-trip
        # correctly through Save Project / Load Project.
        field_names = {f.name for f in DeviceConfig.__dataclass_fields__.values()}
        kwargs = {k: v for k, v in d.items() if k in field_names and k != "points"}
        kwargs["points"] = points
        return DeviceConfig(**kwargs)

    @staticmethod
    def load_esp32_nvs_json(filepath: str) -> "DeviceConfig":
        """
        Import path for your EXISTING ESP32 NVS-export JSON files
        (the {"slave_id":..,"data":[{"Label":..,"Address":..}, ...]} shape).
        Lets you re-use old ESP32 configs as a starting point for a Linux
        board without retyping every point.
        """
        with open(filepath, "r") as f:
            d = json.load(f)
        points = [
            RegisterPoint(label=item["Label"], address=item["Address"],
                          register_type="holding")
            for item in d.get("data", [])
        ]
        return DeviceConfig(
            device_id=d.get("device_id", "AMSET-001"),
            slave_id=d.get("slave_id", 1),
            points=points,
        )

    def validate_all(self) -> List[str]:
        """Returns a list of error strings (empty list = all valid)."""
        errors = []
        used = {}          # (slave, table, register) -> row index of the first user
        labels = {}
        for i, p in enumerate(self.points):
            err = p.validate()
            if err:
                errors.append(f"Row {i+1} ({p.label or '?'}): {err}")
                continue
            if p.label in labels:
                errors.append(f"Row {i+1}: duplicate label '{p.label}' (also row {labels[p.label]+1}); "
                              f"labels are the telemetry keys")
            labels.setdefault(p.label, i)
            slave = p.slave_id if p.slave_id > 0 else self.slave_id
            width = REGISTER_WIDTH[p.data_type]
            for reg in range(p.address, p.address + width):
                key = (slave, p.register_type, reg)
                if key in used:
                    errors.append(
                        f"Row {i+1}: {p.data_type} at {p.address} overlaps register {reg} "
                        f"(slave {slave}, {p.register_type}) of row {used[key]+1}"
                    )
                    break
            for reg in range(p.address, p.address + width):
                used.setdefault((slave, p.register_type, reg), i)
        return errors
