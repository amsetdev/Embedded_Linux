"""core/excel_import.py: register lists from vendor spreadsheets.

Each test writes a real .xlsx with openpyxl and imports it.
"""

import pytest
from openpyxl import Workbook

from core.excel_import import import_excel
from core.register_model import VALID_DATA_TYPES, VALID_REG_TYPES

pytestmark = pytest.mark.unit


@pytest.fixture
def xlsx(tmp_path):
    def make(rows, name="regs.xlsx"):
        wb = Workbook()
        ws = wb.active
        for row in rows:
            ws.append(row)
        path = tmp_path / name
        wb.save(path)
        return str(path)
    return make


def test_minimal_two_columns(xlsx):
    points, warnings = import_excel(xlsx([["Label", "Address"], ["P1", 10], ["P2", 11]]))
    assert [(p.label, p.address, p.slave_id, p.register_type, p.data_type) for p in points] == [
        ("P1", 10, 0, "holding", "uint16"), ("P2", 11, 0, "holding", "uint16")]
    assert warnings == []


@pytest.mark.parametrize("headers", [
    ["Name", "Addr", "Slave", "Reg Type", "DType", "Units"],
    ["  TAG ", "Register Address", "slave_id", "register_type", "datatype", "unit"],
    ["Point Name", "Register", "SlaveID", "Type", "Data Type", "Unit"],
])
def test_header_aliases(xlsx, headers):
    points, _ = import_excel(xlsx([headers, ["P1", 7, 3, "input", "float32", "bar"]]))
    p = points[0]
    assert (p.label, p.address, p.slave_id, p.register_type, p.data_type, p.unit) == ("P1", 7, 3, "input", "float32", "bar")


def test_missing_required_column_is_an_error(xlsx):
    with pytest.raises(ValueError, match="Label"):
        import_excel(xlsx([["Tag Description", "Address"], ["x", 1]]))


@pytest.mark.parametrize("raw, expected", [
    ("Holding Register", "holding"), ("COIL STATUS", "coil"), ("Discrete Input", "discrete"),
    ("Input Register", "input"), ("input", "input"),
])
def test_register_type_synonyms(xlsx, raw, expected):
    points, warnings = import_excel(xlsx([["Label", "Address", "Type"], ["P", 1, raw]]))
    assert points[0].register_type == expected and warnings == []


@pytest.mark.parametrize("raw, expected", [
    ("FLOAT", "float32"), ("Float32", "float32"), ("INT32", "int32"), ("word", "uint16"), ("u16", "uint16"),
])
def test_data_type_synonyms(xlsx, raw, expected):
    points, _ = import_excel(xlsx([["Label", "Address", "Data Type"], ["P", 1, raw]]))
    assert points[0].data_type == expected


@pytest.mark.parametrize("raw", ["INT16", "int16", "Signed Int16", "INT", "SHORT"])
def test_int16_not_imported_as_int32(xlsx, raw):
    """Regression: any type containing "int" became int32, so a vendor INT16 register was
    read as two registers (wrong value, overlapping the next register)."""
    points, warnings = import_excel(xlsx([["Label", "Address", "Data Type"], ["P", 1, raw]]))
    assert points[0].data_type == "uint16"
    assert len(warnings) == 1 and "negative values read as 65536 + value" in warnings[0]


@pytest.mark.parametrize("raw, expected", [
    ("UINT", "uint16"), ("WORD", "uint16"), ("UINT16", "uint16"),
    ("DINT", "int32"), ("INT32", "int32"), ("Long", "int32"),
    ("REAL", "float32"), ("Single", "float32"),
])
def test_iec_61131_type_names(xlsx, raw, expected):
    points, warnings = import_excel(xlsx([["Label", "Address", "Data Type"], ["P", 1, raw]]))
    assert points[0].data_type == expected and warnings == []


@pytest.mark.parametrize("raw", ["UDINT", "DWORD", "uint32"])
def test_unsigned_32bit_imported_with_warning(xlsx, raw):
    points, warnings = import_excel(xlsx([["Label", "Address", "Data Type"], ["P", 1, raw]]))
    assert points[0].data_type == "int32"
    assert "above 2147483647 read as negative" in warnings[0]


def test_unknown_types_default_with_one_warning_each(xlsx):
    points, warnings = import_excel(xlsx([["Label", "Address", "Type", "Data Type"], ["P", 1, "weird", "bcd"]]))
    assert (points[0].register_type, points[0].data_type) == ("holding", "uint16")
    assert len(warnings) == 2 and "unrecognized register type 'weird'" in warnings[0]


def test_bad_rows_skipped_with_row_numbers(xlsx):
    points, warnings = import_excel(xlsx([
        ["Label", "Address", "Slave"],
        ["OK1", 1, 1],
        [None, 2, 1],          # row 3: empty label
        ["NOADDR", None, 1],   # row 4
        ["TEXT", "forty", 1],  # row 5
        ["BADSLAVE", 6, 300],  # row 6: kept, slave defaulted
        ["OK2", "7.0", "2"],
    ]))
    assert [p.label for p in points] == ["OK1", "BADSLAVE", "OK2"]
    assert [p.slave_id for p in points] == [1, 0, 2]
    assert points[2].address == 7
    assert [w.split(":")[0].split(" (")[0] for w in warnings] == ["Row 3", "Row 4", "Row 5", "Row 6"]


def test_blank_rows_ignored(xlsx):
    points, warnings = import_excel(xlsx([["Label", "Address"], ["A", 1], [None, None], ["B", 2]]))
    assert [p.label for p in points] == ["A", "B"] and warnings == []


def test_imported_points_are_valid_for_the_model(xlsx):
    points, _ = import_excel(xlsx([["Label", "Address", "Type", "Data Type"],
                                   ["A", 1, "Coil", "bool"], ["B", 2, "Input Register", "Float"]]))
    for p in points:
        assert p.register_type in VALID_REG_TYPES and p.data_type in VALID_DATA_TYPES
        assert p.validate() is None
