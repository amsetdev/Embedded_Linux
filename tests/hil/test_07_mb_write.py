"""Modbus write commands over MQTT (mb_cmd.h), end to end: a command published on a
local broker (local_broker.py, reached by the board through the SSH connection) is
written by the gateway over the real RS485 bus into the simulated slave, answered on
the response topic, and read back in the gateway's telemetry. Refused commands put
nothing on the bus."""

import json
import struct

import pytest

from conftest import DEVICE_ID, RTU_REGISTERS, base_config, rtu_available
from local_broker import LocalBroker
from sim import COILS, HOLDING

pytestmark = pytest.mark.hardware

TELEMETRY = f"hil/{DEVICE_ID}/telemetry"
CMD = f"devices/{DEVICE_ID}/commands"
RESP = f"devices/{DEVICE_ID}/commands/response"


@pytest.fixture(scope="module")
def link(board, gateway, slaves):
    if not rtu_available():
        pytest.skip("no RS485 adapter (HIL_RTU_PORT)")
    with LocalBroker(board) as broker:
        broker.install_device_certs()
        observer = broker.observer([TELEMETRY, RESP])
        cfg = base_config([r for r, _ in RTU_REGISTERS], mqtt=broker.mqtt_section(DEVICE_ID, TELEMETRY))
        mark = gateway.apply_config(cfg)
        board.wait_log(mark, rf"\[MQTT\] Subscribed to {CMD}$", timeout=90)
        try:
            yield observer
        finally:
            observer.publish(CMD, b"", retain=True)       # never leave a retained command behind
            observer.close()
            for reg, value in RTU_REGISTERS:              # the other tests expect the original values
                slaves.load(reg, value)


def send(observer, request_id, method, retain=False, **params):
    observer.publish(CMD, json.dumps({"requestId": request_id, "method": method, "params": params}),
                     retain=retain)
    return observer.wait_json(RESP, lambda d: d.get("requestId") == request_id, timeout=30)


def holding(slaves, slave, addr, count=1):
    return slaves.rtu[slave].getValues(HOLDING, addr, count)


def test_fc06_written_to_the_slave(link, slaves):
    resp = send(link, "w1", "mb_write_single", slave=1, fc=6, addr=10, value=777)
    assert resp == {"requestId": "w1", "method": "mb_write_single", "status": "ok"}
    assert holding(slaves, 1, 10) == [777]


def test_fc06_reaches_the_right_slave(link, slaves):
    assert send(link, "w2", "mb_write_single", slave=2, fc=6, addr=10, value=4242)["status"] == "ok"
    assert holding(slaves, 2, 10) == [4242]
    assert holding(slaves, 1, 10) != [4242]


def test_fc16_int32_written_and_read_back(link, slaves):
    raw = 1_000_000 & 0xFFFFFFFF
    resp = send(link, "w3", "mb_write_multiple", slave=1, fc=16, addr=20, count=2,
                values=[raw >> 16, raw & 0xFFFF])
    assert resp["status"] == "ok", resp
    assert holding(slaves, 1, 20, 2) == [raw >> 16, raw & 0xFFFF]
    # the gateway's next poll reads the new value back
    msg = link.wait_json(TELEMETRY, lambda d: d.get("values", {}).get("S1_HOLD_I32") == 1_000_000, timeout=60)
    assert msg["values"]["S1_HOLD_I32"] == 1_000_000


def test_fc16_float32_written(link, slaves):
    hi, lo = struct.unpack(">HH", struct.pack(">f", -12.5))
    assert send(link, "w4", "mb_write_multiple", slave=1, fc=16, addr=30, count=2, values=[hi, lo])["status"] == "ok"
    assert holding(slaves, 1, 30, 2) == [hi, lo]


def test_fc05_coil_written(link, slaves):
    assert send(link, "w5", "mb_write_single", slave=1, fc=5, addr=60, value=0)["status"] == "ok"
    assert slaves.rtu[1].getValues(COILS, 60, 1) == [False]
    assert send(link, "w6", "mb_write_single", slave=1, fc=5, addr=60, value=1)["status"] == "ok"
    assert slaves.rtu[1].getValues(COILS, 60, 1) == [True]


@pytest.mark.parametrize("params, detail", [
    (dict(slave=1, fc=6, addr=11, value=1), "slave 1 addr 11 is not a configured holding register"),
    (dict(slave=1, fc=6, addr=40, value=1), "S1_IN_U16, an input register"),
    (dict(slave=1, fc=6, addr=20, value=1), "write both words with fc 16"),
    (dict(slave=3, fc=6, addr=10, value=1), "slave 3 addr 10 is not a configured holding register"),
], ids=["unconfigured", "input-register", "half-int32", "unknown-slave"])
def test_refused_writes_put_nothing_on_the_bus(link, slaves, board, params, detail):
    before = {(s, a): holding(slaves, s, a) for s, a in ((1, 11), (1, 40), (1, 20), (1, 21))}
    mark = board.mark()
    resp = send(link, f"r-{params['addr']}-{params['slave']}", "mb_write_single", **params)
    assert resp["status"] == "error" and detail in resp["detail"], resp
    assert {(s, a): holding(slaves, s, a) for s, a in before} == before
    assert "[DATA] Write" not in board.since(mark)


def test_retained_command_not_replayed_on_reconnect(link, slaves, board):
    # Live, a retained publish reaches subscribers with retain=0 and is executed once.
    assert send(link, "w7", "mb_write_single", retain=True, slave=1, fc=6, addr=10, value=9)["status"] == "ok"
    assert holding(slaves, 1, 10) == [9]
    # After a reconnect the broker replays it with retain=1: answered, never written again.
    slaves.rtu[1].setValues(HOLDING, 10, [5])
    mark = board.mark()
    board.service("restart")
    board.wait_log(mark, rf"\[MQTT\] Subscribed to {CMD}$", timeout=90)
    resp = link.wait_json(RESP, lambda d: d.get("requestId") == "w7", timeout=30)
    assert resp["status"] == "error" and resp["detail"] == "retained commands are not executed"
    link.publish(CMD, b"", retain=True)
    assert holding(slaves, 1, 10) == [5]
    assert "[DATA] Write" not in board.since(mark)


def test_malformed_command_answered(link):
    link.publish(CMD, b'{"requestId":"m1","method":"mb_write_single","params":{"slave":1}}')
    resp = link.wait_json(RESP, lambda d: d.get("requestId") == "m1", timeout=30)
    assert resp["status"] == "error" and "missing params.fc" in resp["detail"]
