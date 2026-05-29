#!/usr/bin/env python3
"""
Modbus TCP Slave (simulator)
Listens on 0.0.0.0:502  –  slave id 1
Holding registers 1..100 pre-filled with test values
"""

from pymodbus.server import StartTcpServer
from pymodbus.datastore import (
    ModbusServerContext,
    ModbusSequentialDataBlock,
    ModbusSlaveContext,
)

# ── Pre-fill holding registers 0..101 with recognisable values ──
# Index 0 = address 0, index 1 = address 1 (what your C code reads)
hr_values = [0] + [i * 10 for i in range(1, 102)]   # reg[1]=10, reg[2]=20 …

store = ModbusSlaveContext(
    di=ModbusSequentialDataBlock(0, [0] * 110),   # discrete inputs
    co=ModbusSequentialDataBlock(0, [0] * 110),   # coils
    hr=ModbusSequentialDataBlock(0, hr_values),   # holding registers ← your C master reads these
    ir=ModbusSequentialDataBlock(0, [0] * 110),   # input registers
)

context = ModbusServerContext(slaves=store, single=True)

print("Modbus TCP Slave listening on 0.0.0.0:502  (slave id=1)")
print("Holding registers 1-100 filled: reg[N] = N * 10")

StartTcpServer(
    context=context,
    address=("0.0.0.0", 502),
)
