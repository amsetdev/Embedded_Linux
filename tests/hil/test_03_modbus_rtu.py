"""Modbus RTU over the board's RS485 against the simulated slaves (sim.py) on the
RS485 adapter; values checked in the telemetry payload the gateway stores while
offline (the same JSON it publishes: the cloud contract)."""

import pytest

from conftest import RTU_EXPECTED, RTU_REGISTERS, base_config, rtu_available

pytestmark = pytest.mark.hardware

MISSING = {"label": "S3_ABSENT", "address": 5, "type": "holding", "data_type": "uint16", "slave_id": 3}


@pytest.fixture(scope="module")
def telemetry(gateway, slaves):
    if not rtu_available():
        pytest.skip("HIL_RTU_PORT (RS485 adapter wired to the board) not available")
    gateway.apply_config(base_config([r for r, _ in RTU_REGISTERS] + [MISSING]))
    gateway.clear_storage()
    (_, first), (_, second) = gateway.wait_stored(count=2, timeout=120)
    return second          # the first cycle may have started before the slaves answered


def test_every_register_read_with_the_slave_value(telemetry):
    values = telemetry["values"]
    for label, want in RTU_EXPECTED.items():
        assert label in values, f"{label} missing (read failed)"
        assert values[label] == pytest.approx(want), label


def test_32bit_values_high_word_first(telemetry):
    assert telemetry["values"]["S1_HOLD_I32"] == -123456
    assert telemetry["values"]["S2_HOLD_I32_BIG"] == 16777217        # above 2^24: exact


def test_discrete_input_read_from_discrete_table(telemetry):
    """The slave's coil 70 holds the opposite value: 1 proves function code 02 was used."""
    assert telemetry["values"]["S1_DISC"] == 1


def test_failed_read_left_out(telemetry):
    """Slave 3 doesn't exist: its register is left out of the message, the rest is sent."""
    assert "S3_ABSENT" not in telemetry["values"]


def test_payload_contract(telemetry):
    assert set(telemetry) == {"ts", "values"}
    assert isinstance(telemetry["ts"], int) and telemetry["ts"] > 1_700_000_000_000
