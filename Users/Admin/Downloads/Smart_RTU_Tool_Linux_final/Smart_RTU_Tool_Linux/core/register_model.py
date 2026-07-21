"""
register_model.py
------------------
Defines the RegisterPoint data model and the DeviceConfig container that
holds everything the tool needs to build a registers.csv for the
STM32MP157F-DK2 Modbus RTU firmware.

This replaces the ESP32 tool's NVS JSON model. The on-disk *interchange*
format stays JSON (so you can still save/load "projects" in the tool),
but the *deployment* format sent to the board is always registers.csv,
matching data.h's parse_csv() expectations:

    label,address,register_type,unit

register_type must be one of: holding, input, coil, discrete
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

    # ---- CSV (deployment format) ----------------------------------
    def to_csv_string(self) -> str:
        """
        Builds the exact registers.csv format your firmware's parse_csv()
        expects. NOTE: current firmware (data.c) only reads label/address/
        register_type/unit and treats every point as a single 16-bit word
        (data_type + scale are carried in the tool/JSON for future firmware
        support, e.g. int32/float32 spanning 2 registers, but are NOT yet
        consumed by parse_csv()). They are written as informational extra
        columns and are safely ignored by the current C parser.
        """
        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow(["label", "address", "register_type", "unit",
                          "data_type", "scale"])
        for p in self.points:
            writer.writerow([p.label, p.address, p.register_type,
                              p.unit, p.data_type, p.scale])
        return buf.getvalue()

    def save_csv(self, filepath: str):
        with open(filepath, "w", newline="") as f:
            f.write(self.to_csv_string())

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
        return DeviceConfig(
            device_id=d.get("device_id", "AMSET-001"),
            slave_id=d.get("slave_id", 1),
            baud=d.get("baud", 9600),
            interval_sec=d.get("interval_sec", 30),
            points=points,
        )

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
