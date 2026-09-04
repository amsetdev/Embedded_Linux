"""
register_model.py
------------------
Defines the RegisterPoint data model and the DeviceConfig container that
holds everything the tool needs about one device: Wi-Fi, device/Modbus
settings, Modbus Map Sizing, and the register list.

Everything is delivered to the board as ONE combined file (not three
separate files) — Wi-Fi + Device Settings + Modbus Map Sizing +
Registers all together, in both a .csv and a .json version of the same
content, both written to /home/root/edb_c/linking/ as:

    smart_rtu_config.csv
    smart_rtu_config.json

See FIRMWARE_NOTES.md for what the board-side code needs to read this
combined file instead of the old three separate files.
"""

from __future__ import annotations
import csv
import io
import json
from dataclasses import dataclass, field, asdict
from typing import List, Optional


VALID_REG_TYPES = {"holding", "input", "coil", "discrete"}
VALID_DATA_TYPES = {"word", "int32", "float32"}  # word = single 16-bit register


@dataclass
class RegisterPoint:
    label: str
    address: int
    register_type: str = "holding"     # holding | input | coil | discrete
    data_type: str = "word"            # word | int32 | float32 (see note below)
    unit: str = ""
    scale: float = 1.0

    def validate(self) -> Optional[str]:
        """Returns an error string if invalid, else None."""
        if not self.label or not self.label.strip():
            return "Label cannot be empty"
        if self.address < 0 or self.address > 65535:
            return f"Address {self.address} out of range (0-65535)"
        if self.register_type not in VALID_REG_TYPES:
            return f"Invalid register_type '{self.register_type}'"
        if self.data_type not in VALID_DATA_TYPES:
            return f"Invalid data_type '{self.data_type}'"
        return None


@dataclass
class DeviceConfig:
    """Everything about one target device's configuration."""
    device_id: str = "AMSET-001"
    slave_id: int = 1
    baud: int = 9600
    interval_sec: int = 30
    points: List[RegisterPoint] = field(default_factory=list)

    # ---- Wi-Fi (board's own network, if applicable) -------------------
    wifi_ssid: str = ""
    wifi_password: str = ""

    # ---- Modbus TCP (optional second protocol path) ------------------
    modbus_tcp_enable: int = 0
    modbus_tcp_ip: str = ""
    modbus_tcp_port: int = 502
    modbus_tcp_slave_id: int = 1

    # ---- Board MQTT (AWS IoT Core mTLS) ------------------------------
    mqtt_broker: str = ""
    mqtt_port: int = 8883
    mqtt_client_id: str = ""
    mqtt_ca_cert: str = "/home/root/edb_c/linking/ca.crt"
    mqtt_device_cert: str = "/home/root/edb_c/linking/client.crt"
    mqtt_private_key: str = "/home/root/edb_c/linking/private.key"
    mqtt_topic: str = ""

    # ---- Modbus map sizing (counts only; actual points still come from
    # the Data Transmission table / Excel import) ---------------------
    coils: int = 0
    alerts: int = 0
    holding_integers: int = 0
    holding_decimals: int = 0
    holding_double_integers: int = 0
    input_integers: int = 0
    input_decimals: int = 0
    input_double_integers: int = 0
    parity: str = "None"
    stop_bits: int = 1

    # ---- Auto-assign register_type/data_type from Device Config sizing
    # -------------------------------------------------------------------
    # Register Type / Data Type / Unit / Scale are no longer entered per
    # row in the Data Transmission table. Instead, the counts set on the
    # Device Config page (Holding Integers/Decimals/Double Integers,
    # Input Integers/Decimals/Double Integers) determine how the points
    # — taken in table order — are typed. Example: Holding Integers=6,
    # Holding Decimals=4, with 10 rows in the table -> first 6 rows are
    # holding/word, next 4 are holding/float32.
    def assign_types_from_sizing(self) -> List[str]:
        """Mutates self.points' register_type/data_type in place based on
        the sizing counts above. Returns a list of warning strings (empty
        if the point count matches the configured sizing exactly)."""
        plan = [
            ("holding", "word", self.holding_integers),
            ("holding", "float32", self.holding_decimals),
            ("holding", "int32", self.holding_double_integers),
            ("input", "word", self.input_integers),
            ("input", "float32", self.input_decimals),
            ("input", "int32", self.input_double_integers),
        ]
        idx = 0
        for reg_type, data_type, count in plan:
            for _ in range(count):
                if idx >= len(self.points):
                    break
                self.points[idx].register_type = reg_type
                self.points[idx].data_type = data_type
                idx += 1

        warnings = []
        total_sized = sum(c for _, _, c in plan)
        if idx < len(self.points):
            leftover = len(self.points) - idx
            warnings.append(
                f"{leftover} row(s) exceed the configured Modbus Map Sizing "
                f"({total_sized} total) — left as holding/word. Increase the "
                f"counts on Device Config if these are intentional."
            )
        elif total_sized > len(self.points):
            warnings.append(
                f"Modbus Map Sizing expects {total_sized} point(s) but the "
                f"table only has {len(self.points)} — add more rows or "
                f"reduce the counts on Device Config."
            )
        return warnings

    # ---- Combined config file (Wi-Fi + Device + Modbus Map Sizing +
    # Registers, ALL IN ONE FILE) -------------------------------------
    def to_full_config_dict(self) -> dict:
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
            "modbus_sizing": {
                "coils": self.coils,
                "alerts": self.alerts,
                "holding_integers": self.holding_integers,
                "holding_decimals": self.holding_decimals,
                "holding_double_integers": self.holding_double_integers,
                "input_integers": self.input_integers,
                "input_decimals": self.input_decimals,
                "input_double_integers": self.input_double_integers,
            },
            # Just label + address, same as the Excel sheet. Which bucket
            # (integer/decimal/double, holding/input) each row falls into
            # is worked out from position + the modbus_sizing counts
            # above — call assign_types_from_sizing() if you need that
            # per-row detail elsewhere in the tool.
            "registers": [{"label": p.label, "address": p.address} for p in self.points],
        }

    def sizing_summary(self) -> str:
        """One-line human summary of the Modbus Map Sizing counts, used on
        the Status page recap and in log messages."""
        parts = []
        if self.holding_integers or self.holding_decimals or self.holding_double_integers:
            parts.append(
                f"Holding: {self.holding_integers} int, "
                f"{self.holding_decimals} decimal, "
                f"{self.holding_double_integers} double-int"
            )
        if self.input_integers or self.input_decimals or self.input_double_integers:
            parts.append(
                f"Input: {self.input_integers} int, "
                f"{self.input_decimals} decimal, "
                f"{self.input_double_integers} double-int"
            )
        if self.coils:
            parts.append(f"Coils: {self.coils}")
        if self.alerts:
            parts.append(f"Alerts: {self.alerts}")
        return "; ".join(parts) if parts else "No Modbus Map Sizing configured"

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
        ONE CSV containing everything: Wi-Fi, Device Settings, Modbus
        Map Sizing, and the register list (label, address — same as
        the Excel format). Sections are marked with [SECTION] header
        rows so it stays a single plain CSV file, readable in Excel or
        a text editor. This is the same file sent to the device.
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

        writer.writerow(["[MODBUS_SIZING]"])
        for k, v in d["modbus_sizing"].items():
            writer.writerow([k, v])
        writer.writerow([])

        writer.writerow(["[REGISTERS]"])
        writer.writerow(["label", "address"])
        for r in d["registers"]:
            writer.writerow([r["label"], r["address"]])

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
        seen_addr = {}
        for i, p in enumerate(self.points):
            err = p.validate()
            if err:
                errors.append(f"Row {i+1} ({p.label or '?'}): {err}")
            key = (p.register_type, p.address)
            if key in seen_addr:
                errors.append(
                    f"Row {i+1}: duplicate address {p.address} "
                    f"({p.register_type}) also used in row {seen_addr[key]+1}"
                )
            else:
                seen_addr[key] = i
        return errors
