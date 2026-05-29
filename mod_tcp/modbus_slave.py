#!/usr/bin/env python3
"""
Modbus TCP Slave - pymodbus 3.6.9
Port 502 | Holding registers 1..100 filled with N*10
"""

from pymodbus.server import StartTcpServer
from pymodbus.device import ModbusDeviceIdentification
from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusSlaveContext,
    ModbusServerContext,
)

# Holding registers layout (0-based index):
#   index 0          = address 0  (unused padding)
#   index 1..100     = address 1..100  → values 10, 20, 30 ... 1000
#
# Total size must be >= start_address + count
# libmodbus reads: start=0, count=100  → needs indices 0..99  (100 entries)
# We allocate 110 to be safe.

hr_values = [i * 10 for i in range(110)]   # [0, 10, 20, 30 ... 1090]
#            ^index 0 = 0
#                   ^index 1 = 10  (register address 1)
#                        ^index 2 = 20 (register address 2)  etc.

store = ModbusSlaveContext(
    di=ModbusSequentialDataBlock(0, [0] * 110),
    co=ModbusSequentialDataBlock(0, [0] * 110),
    hr=ModbusSequentialDataBlock(0, hr_values),   # ← 110 registers, addr 0..109
    ir=ModbusSequentialDataBlock(0, [0] * 110),
)

context = ModbusServerContext(slaves=store, single=True)

identity = ModbusDeviceIdentification()
identity.VendorName  = 'STM32 Test'
identity.ProductName = 'Modbus Slave Simulator'

print("=" * 50)
print("  Modbus TCP Slave")
print("  Port    : 502")
print("  HR[0]   = 0")
print("  HR[1]   = 10  (register address 1)")
print("  HR[100] = 1000 (register address 100)")
print("=" * 50)

StartTcpServer(
    context=context,
    identity=identity,
    address=("0.0.0.0", 502),
)