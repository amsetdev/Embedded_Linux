"""The sample project and register sheet at the repo root (smart_rtu_config.json,
test_stm32mp1.xlsx, from feat/mb_write) stay loadable by the tool and agree with
each other. What the device receives from this project is checked by the validate
stage (tests/validate/test_config_files.py)."""

import json

import pytest

from conftest import REPO
from core.excel_import import import_excel
from core.register_model import DeviceConfig

pytestmark = pytest.mark.unit

PROJECT = REPO / "smart_rtu_config.json"
SHEET = REPO / "test_stm32mp1.xlsx"


def key(p):
    return (p.slave_id, p.label, p.address, p.register_type, p.data_type)


def test_project_file_loads():
    config = DeviceConfig.load_json(str(PROJECT))
    assert config.device_id == "loop_1"
    assert len(config.points) == 69
    assert config.validate_all() == []


def test_sheet_imports_without_warnings():
    points, warnings = import_excel(str(SHEET))
    assert len(points) == 69 and warnings == []


def test_sheet_and_project_hold_the_same_registers():
    points, _ = import_excel(str(SHEET))
    project = DeviceConfig.load_json(str(PROJECT))
    assert [key(p) for p in points] == [key(p) for p in project.points]


def test_save_project_keeps_the_file_format(tmp_path):
    """Load → Save writes the same keys and values (plus the fields newer tools add)."""
    original = json.loads(PROJECT.read_text())
    out = tmp_path / "saved.json"
    DeviceConfig.load_json(str(PROJECT)).save_json(str(out))
    saved = json.loads(out.read_text())
    assert set(saved) - set(original) <= {"wifi_enable", "wifi_country"}
    assert {k: saved[k] for k in original} == original
