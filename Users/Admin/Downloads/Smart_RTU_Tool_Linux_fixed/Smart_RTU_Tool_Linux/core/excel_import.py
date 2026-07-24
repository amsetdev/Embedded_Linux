"""
excel_import.py
-----------------
Reads a register list from an Excel file (.xlsx) and converts it into
RegisterPoint objects, ready to be turned into registers.csv for the
board. This replaces manual row-by-row entry for large point counts —
a plant engineer typically already has the register map in a
spreadsheet from the device vendor.

Column matching is flexible (case-insensitive, whitespace-tolerant),
same philosophy as the firmware's own parse_csv() header matching:

    label / name / point / tag        -> label            (required)
    address / addr / reg / register   -> address           (required)
    type / register_type / reg_type   -> register_type      (optional, default "holding")
    unit / units                      -> unit               (optional)
    data_type / datatype / dtype      -> data_type          (optional, default "word")
    scale / multiplier / factor       -> scale              (optional, default 1.0)

Only 'label' and 'address' are required columns — everything else has
a safe default so a minimal two-column spreadsheet (Label, Address)
still imports cleanly.

Requires: pandas, openpyxl
"""

from __future__ import annotations
from typing import List, Tuple
import pandas as pd

from core.register_model import RegisterPoint, VALID_REG_TYPES, VALID_DATA_TYPES


# header aliases -> canonical field name
_HEADER_ALIASES = {
    "label": "label", "name": "label", "point": "label", "tag": "label",
    "point name": "label", "pointname": "label",

    "address": "address", "addr": "address", "reg": "address",
    "register": "address", "register address": "address",

    "type": "register_type", "register type": "register_type",
    "register_type": "register_type", "reg type": "register_type",
    "reg_type": "register_type",

    "unit": "unit", "units": "unit",

    "data type": "data_type", "data_type": "data_type",
    "datatype": "data_type", "dtype": "data_type",

    "scale": "scale", "multiplier": "scale", "factor": "scale",
}


def _normalize_header(h: str) -> str:
    return str(h).strip().lower()


def _map_columns(columns) -> dict:
    """Returns {canonical_field: actual_column_name} for whatever
    matches in this sheet's header row."""
    mapping = {}
    for col in columns:
        norm = _normalize_header(col)
        if norm in _HEADER_ALIASES:
            canonical = _HEADER_ALIASES[norm]
            mapping.setdefault(canonical, col)
    return mapping


def import_excel(filepath: str, sheet_name=0) -> Tuple[List[RegisterPoint], List[str]]:
    """
    Returns (points, warnings). Raises ValueError if required columns
    (label, address) can't be found at all — that's a hard failure,
    since there's nothing sensible to import.
    """
    df = pd.read_excel(filepath, sheet_name=sheet_name, dtype=str)
    df = df.dropna(how="all")  # drop fully blank rows

    colmap = _map_columns(df.columns)
    if "label" not in colmap or "address" not in colmap:
        raise ValueError(
            "Could not find required columns in this sheet. "
            "Need at least a 'Label' (or Name/Point/Tag) column and an "
            "'Address' (or Addr/Register) column. "
            f"Found columns: {list(df.columns)}"
        )

    points: List[RegisterPoint] = []
    warnings: List[str] = []

    for i, row in df.iterrows():
        excel_row_num = i + 2  # +1 for 0-index, +1 for header row

        label_val = row.get(colmap["label"])
        addr_val = row.get(colmap["address"])

        if pd.isna(label_val) or str(label_val).strip() == "":
            warnings.append(f"Row {excel_row_num}: empty label, skipped")
            continue
        label = str(label_val).strip()

        if pd.isna(addr_val) or str(addr_val).strip() == "":
            warnings.append(f"Row {excel_row_num} ({label}): empty address, skipped")
            continue
        try:
            address = int(float(str(addr_val).strip()))
        except ValueError:
            warnings.append(
                f"Row {excel_row_num} ({label}): address '{addr_val}' is not "
                f"a number, skipped"
            )
            continue

        register_type = "holding"
        if "register_type" in colmap:
            raw = row.get(colmap["register_type"])
            if not pd.isna(raw) and str(raw).strip():
                rt = str(raw).strip().lower()
                if rt in VALID_REG_TYPES:
                    register_type = rt
                elif "coil" in rt:
                    register_type = "coil"
                elif "discrete" in rt:
                    register_type = "discrete"
                elif "input" in rt:
                    register_type = "input"
                elif "holding" in rt:
                    register_type = "holding"
                else:
                    warnings.append(
                        f"Row {excel_row_num} ({label}): unrecognized "
                        f"register type '{raw}', defaulted to 'holding'"
                    )

        data_type = "word"
        if "data_type" in colmap:
            raw = row.get(colmap["data_type"])
            if not pd.isna(raw) and str(raw).strip():
                dt = str(raw).strip().lower()
                if dt in VALID_DATA_TYPES:
                    data_type = dt
                elif "float" in dt:
                    data_type = "float32"
                elif "int" in dt or "32" in dt:
                    data_type = "int32"
                else:
                    warnings.append(
                        f"Row {excel_row_num} ({label}): unrecognized "
                        f"data type '{raw}', defaulted to 'word'"
                    )

        unit = ""
        if "unit" in colmap:
            raw = row.get(colmap["unit"])
            if not pd.isna(raw):
                unit = str(raw).strip()

        scale = 1.0
        if "scale" in colmap:
            raw = row.get(colmap["scale"])
            if not pd.isna(raw) and str(raw).strip():
                try:
                    scale = float(raw)
                except ValueError:
                    warnings.append(
                        f"Row {excel_row_num} ({label}): scale '{raw}' is not "
                        f"a number, defaulted to 1.0"
                    )

        points.append(RegisterPoint(
            label=label, address=address, register_type=register_type,
            data_type=data_type, unit=unit, scale=scale,
        ))

    return points, warnings
