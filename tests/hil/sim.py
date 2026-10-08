"""Simulated Modbus slaves for the HIL tests (pymodbus 3.6): an RTU server on the
RS485 adapter and a TCP server on this PC, both in ONE asyncio loop in a thread
(pymodbus' Start*/ServerStop helpers only handle one server at a time).

Tables use zero_mode: address N in the gateway config is register N here.
"""

import asyncio
import struct
import threading

from pymodbus.datastore import ModbusSequentialDataBlock, ModbusServerContext, ModbusSlaveContext
from pymodbus.server import ModbusSerialServer, ModbusTcpServer
from pymodbus.transaction import ModbusRtuFramer

COILS, DISCRETE, HOLDING, INPUT = 1, 2, 3, 4      # pymodbus function-code table ids
TABLE = {"coil": COILS, "discrete": DISCRETE, "holding": HOLDING, "input": INPUT}
SIZE = 1024


def slave_context():
    return ModbusSlaveContext(
        co=ModbusSequentialDataBlock(0, [0] * SIZE), di=ModbusSequentialDataBlock(0, [0] * SIZE),
        hr=ModbusSequentialDataBlock(0, [0] * SIZE), ir=ModbusSequentialDataBlock(0, [0] * SIZE),
        zero_mode=True)


def to_registers(data_type, value):
    """Register words as the gateway reads them: 32-bit values high word first."""
    if data_type == "float32":
        raw = struct.unpack(">I", struct.pack(">f", value))[0]
    elif data_type == "int32":
        raw = value & 0xFFFFFFFF
    else:
        return [value & 0xFFFF]
    return [raw >> 16, raw & 0xFFFF]


class Slaves:
    def __init__(self, slave_ids=(1, 2)):
        self.rtu = ModbusServerContext(single=False, slaves={s: slave_context() for s in slave_ids})
        self.tcp = ModbusServerContext(single=True, slaves=slave_context())
        self.loop = None
        self.servers = []
        self.thread = None

    def load(self, reg, value):
        """Put a register of the gateway config (dict with slave_id/address/type/data_type) in the RTU slave."""
        ctx = self.rtu[reg["slave_id"]]
        table = TABLE[reg["type"]]
        if table in (COILS, DISCRETE):
            ctx.setValues(table, reg["address"], [bool(value)])
            if table == DISCRETE:
                ctx.setValues(COILS, reg["address"], [not value])   # shows which function code was used
        else:
            ctx.setValues(table, reg["address"], to_registers(reg["data_type"], value))

    def set_tcp_holding(self, start, values):
        self.tcp[0].setValues(HOLDING, start, list(values))

    def start(self, rtu_port=None, rtu_serial=None, tcp_port=None):
        ready = threading.Event()

        async def main():
            self.loop = asyncio.get_running_loop()
            if rtu_port:
                s = ModbusSerialServer(self.rtu, framer=ModbusRtuFramer, port=rtu_port, **(rtu_serial or {}))
                self.servers.append(s)
            if tcp_port:
                self.servers.append(ModbusTcpServer(self.tcp, address=("127.0.0.1", tcp_port)))
            tasks = [asyncio.create_task(s.serve_forever()) for s in self.servers]
            await asyncio.sleep(0.5)
            self.errors = [t.exception() for t in tasks if t.done() and t.exception()]
            ready.set()
            await asyncio.gather(*tasks, return_exceptions=True)

        self.thread = threading.Thread(target=lambda: asyncio.run(main()), daemon=True)
        self.thread.start()
        if not ready.wait(10):
            raise RuntimeError("simulated Modbus slaves did not start")
        if getattr(self, "errors", None):
            raise RuntimeError(f"simulated Modbus slave failed to start: {self.errors!r}")

    def stop(self):
        if not self.loop:
            return
        for s in self.servers:
            asyncio.run_coroutine_threadsafe(s.shutdown(), self.loop).result(timeout=10)
        self.thread.join(timeout=10)
