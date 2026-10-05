"""Register type / data type names agree between the firmware, the tool and the schema.

Firmware: RegType (inc/data.h), reg_type_from_string() / data_type_from_string()
(src/fieldbus/data.c). Tool: VALID_REG_TYPES / VALID_DATA_TYPES
(core/register_model.py) and the REG_TYPES / DATA_TYPES dropdowns
(ui/main_window.py). Schema: the register enums. When one side changes, these
tests say which other places must follow (DOCS/CI_CD_GUIDE.md §7).
"""

import ast
import json
import re

import pytest

from conftest import REPO
from config_checks import DATA_TYPE_CODE, REG_TYPE_CODE, SCHEMA_PATH
from firmware_view import firmware_view
from core.register_model import VALID_DATA_TYPES, VALID_REG_TYPES

pytestmark = pytest.mark.validate

DATA_C = (REPO / "src/fieldbus/data.c").read_text()
DATA_H = (REPO / "inc/data.h").read_text()
SCHEMA = json.loads(SCHEMA_PATH.read_text())
REGISTER_SCHEMA = SCHEMA["$defs"]["register"]["properties"]


def c_function(name):
    m = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{(.*?)\n\}}", DATA_C, re.S)
    assert m, f"{name}() not found in src/fieldbus/data.c"
    return m.group(1)


def c_strcmp_names(func):
    return set(re.findall(r'strcmp\(\s*s\s*,\s*"([^"]+)"\s*\)', c_function(func)))


def ui_list(name):
    tree = ast.parse((REPO / "tools/smart_rtu_tool/ui/main_window.py").read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == name for t in node.targets):
            return ast.literal_eval(node.value)
    pytest.fail(f"{name} not found in ui/main_window.py")


def test_regtype_enum_order_matches_codes():
    body = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*RegType;", DATA_H, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = [n.strip() for n in body.split(",") if n.strip()]
    assert names == ["REG_HOLDING", "REG_INPUT", "REG_COIL", "REG_DISCRETE"]
    assert {f"REG_{k.upper()}": v for k, v in REG_TYPE_CODE.items()} == {n: i for i, n in enumerate(names)}


def test_tool_lists_agree():
    assert set(ui_list("REG_TYPES")) == VALID_REG_TYPES == set(REG_TYPE_CODE)
    assert set(ui_list("DATA_TYPES")) == VALID_DATA_TYPES == set(DATA_TYPE_CODE)


def test_schema_enums_match_tool():
    assert set(REGISTER_SCHEMA["type"]["enum"]) == VALID_REG_TYPES
    assert set(REGISTER_SCHEMA["data_type"]["enum"]) == VALID_DATA_TYPES


def test_firmware_recognises_every_tool_register_type():
    # "holding" is reg_type_from_string()'s fallback, every other name must be matched explicitly
    assert VALID_REG_TYPES - {"holding"} <= c_strcmp_names("reg_type_from_string")


def test_firmware_recognises_every_tool_data_type():
    # "uint16" is data_type_from_string()'s fallback ('w')
    assert VALID_DATA_TYPES - {"uint16"} <= c_strcmp_names("data_type_from_string")


def test_firmware_maps_each_type_as_expected():
    """The real parser turns every (type, data_type) pair into the expected codes."""
    regs = [{"label": f"{t}_{d}", "address": 10 * i + 2 * j, "type": t, "data_type": d, "slave_id": 1}
            for i, t in enumerate(sorted(VALID_REG_TYPES)) for j, d in enumerate(sorted(VALID_DATA_TYPES))]
    view = firmware_view(json.dumps({"device": {"slave_id": 1}, "registers": regs}, indent=2))
    got = [(r["reg_type"], r["data_type"]) for r in view["registers"]]
    assert got == [(REG_TYPE_CODE[r["type"]], DATA_TYPE_CODE[r["data_type"]]) for r in regs]
