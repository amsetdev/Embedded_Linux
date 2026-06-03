#!/usr/bin/env python3
"""
Modbus Master Simulator — compatible with pymodbus 3.6.x
Supports TCP, RTU Serial, ASCII Serial
"""

import sys
import time
import argparse

try:
    from pymodbus.client import ModbusTcpClient, ModbusSerialClient
    from pymodbus.exceptions import ModbusException, ConnectionException
    from pymodbus.framer import ModbusRtuFramer, ModbusAsciiFramer, ModbusSocketFramer
except ImportError:
    print("Error: pymodbus not installed. Run: pip install pymodbus")
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────────────
# ANSI Colors
# ─────────────────────────────────────────────────────────────────────────────
class C:
    RED    = "\033[91m"
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    CYAN   = "\033[96m"
    BOLD   = "\033[1m"
    RESET  = "\033[0m"
    DIM    = "\033[2m"

def ok(msg):   print(f"  {C.GREEN}[OK]{C.RESET}  {msg}")
def err(msg):  print(f"  {C.RED}[ERR]{C.RESET} {msg}")
def info(msg): print(f"  {C.CYAN}[>>]{C.RESET}  {msg}")
def warn(msg): print(f"  {C.YELLOW}[!]{C.RESET}   {msg}")

BANNER = f"""
{C.BOLD}{C.CYAN}╔══════════════════════════════════════════════════╗
║        Modbus Master Simulator  v1.0             ║
║  Supports TCP  •  RTU Serial  •  ASCII Serial    ║
╚══════════════════════════════════════════════════╝{C.RESET}
"""

# ─────────────────────────────────────────────────────────────────────────────
# Connection helpers
# ─────────────────────────────────────────────────────────────────────────────

def connect_tcp(host: str, port: int, timeout: float):
    client = ModbusTcpClient(host=host, port=port, timeout=timeout)
    if not client.connect():
        raise ConnectionException(f"Cannot connect to {host}:{port}")
    ok(f"TCP connected → {host}:{port}")
    return client


def connect_serial(port: str, baudrate: int, parity: str,
                   stopbits: int, bytesize: int, timeout: float, mode: str):
    framer = ModbusRtuFramer if mode.upper() == "RTU" else ModbusAsciiFramer
    client = ModbusSerialClient(
        port=port,
        framer=framer,
        baudrate=baudrate,
        parity=parity,
        stopbits=stopbits,
        bytesize=bytesize,
        timeout=timeout,
    )
    if not client.connect():
        raise ConnectionException(f"Cannot open serial port {port}")
    ok(f"Serial ({mode}) connected → {port} @ {baudrate} baud")
    return client


# ─────────────────────────────────────────────────────────────────────────────
# Display helpers
# ─────────────────────────────────────────────────────────────────────────────

def display_registers(registers, start_addr, label):
    print(f"\n  {C.BOLD}{label}{C.RESET}")
    print(f"  {'Addr':>6}  {'Dec':>6}  {'Hex':>6}  {'Bin':>18}")
    print(f"  {'─'*6}  {'─'*6}  {'─'*6}  {'─'*18}")
    for i, val in enumerate(registers):
        print(f"  {start_addr+i:>6}  {val:>6}  {val:#06x}  {val:>018b}")


def display_bits(bits, start_addr, label):
    print(f"\n  {C.BOLD}{label}{C.RESET}")
    print(f"  {'Addr':>6}  {'State':>5}")
    print(f"  {'─'*6}  {'─'*5}")
    for i, val in enumerate(bits):
        state = f"{C.GREEN}ON {C.RESET}" if val else f"{C.RED}OFF{C.RESET}"
        print(f"  {start_addr+i:>6}  {state}")


# ─────────────────────────────────────────────────────────────────────────────
# Modbus operations
# ─────────────────────────────────────────────────────────────────────────────

def read_holding_registers(client, slave, address, count):
    info(f"FC03 Read Holding Registers | slave={slave} addr={address} count={count}")
    r = client.read_holding_registers(address=address, count=count, slave=slave)
    if r.isError():
        err(str(r)); return
    display_registers(r.registers, address, "Holding Registers (FC03)")


def read_input_registers(client, slave, address, count):
    info(f"FC04 Read Input Registers | slave={slave} addr={address} count={count}")
    r = client.read_input_registers(address=address, count=count, slave=slave)
    if r.isError():
        err(str(r)); return
    display_registers(r.registers, address, "Input Registers (FC04)")


def read_coils(client, slave, address, count):
    info(f"FC01 Read Coils | slave={slave} addr={address} count={count}")
    r = client.read_coils(address=address, count=count, slave=slave)
    if r.isError():
        err(str(r)); return
    display_bits(r.bits[:count], address, "Coils (FC01)")


def read_discrete_inputs(client, slave, address, count):
    info(f"FC02 Read Discrete Inputs | slave={slave} addr={address} count={count}")
    r = client.read_discrete_inputs(address=address, count=count, slave=slave)
    if r.isError():
        err(str(r)); return
    display_bits(r.bits[:count], address, "Discrete Inputs (FC02)")


def write_single_register(client, slave, address, value):
    info(f"FC06 Write Single Register | slave={slave} addr={address} value={value}")
    r = client.write_register(address=address, value=value, slave=slave)
    if r.isError(): err(str(r))
    else: ok(f"Register {address} ← {value}  ({value:#06x})")


def write_multiple_registers(client, slave, address, values):
    info(f"FC16 Write Multiple Registers | slave={slave} addr={address} values={values}")
    r = client.write_registers(address=address, values=values, slave=slave)
    if r.isError(): err(str(r))
    else: ok(f"Registers {address}–{address+len(values)-1} written: {values}")


def write_single_coil(client, slave, address, value):
    info(f"FC05 Write Single Coil | slave={slave} addr={address} value={value}")
    r = client.write_coil(address=address, value=value, slave=slave)
    if r.isError(): err(str(r))
    else: ok(f"Coil {address} ← {'ON' if value else 'OFF'}")


def write_multiple_coils(client, slave, address, values):
    info(f"FC15 Write Multiple Coils | slave={slave} addr={address} values={values}")
    r = client.write_coils(address=address, values=values, slave=slave)
    if r.isError(): err(str(r))
    else: ok(f"Coils {address}–{address+len(values)-1} written")


def mask_write_register(client, slave, address, and_mask, or_mask):
    info(f"FC22 Mask Write | slave={slave} addr={address} AND={and_mask:#06x} OR={or_mask:#06x}")
    r = client.mask_write_register(address=address, and_mask=and_mask, or_mask=or_mask, slave=slave)
    if r.isError(): err(str(r))
    else: ok(f"Mask write applied to register {address}")


def read_write_multiple_registers(client, slave, r_addr, r_count, w_addr, w_vals):
    info(f"FC23 Read/Write | read@{r_addr}×{r_count} write@{w_addr}={w_vals}")
    r = client.readwrite_registers(
        read_address=r_addr, read_count=r_count,
        write_address=w_addr, write_registers=w_vals,
        slave=slave)
    if r.isError(): err(str(r))
    else: display_registers(r.registers, r_addr, "Read/Write Result (FC23)")


def poll_registers(client, slave, address, count, interval, fc):
    fc_map = {1: read_coils, 2: read_discrete_inputs,
              3: read_holding_registers, 4: read_input_registers}
    fn = fc_map.get(fc, read_holding_registers)
    info(f"Polling FC{fc:02d} every {interval}s — Ctrl+C to stop\n")
    try:
        while True:
            print(f"\n{C.DIM}  [{time.strftime('%H:%M:%S')}]{C.RESET}")
            fn(client, slave, address, count)
            time.sleep(interval)
    except KeyboardInterrupt:
        warn("Polling stopped.")


# ─────────────────────────────────────────────────────────────────────────────
# Interactive menu
# ─────────────────────────────────────────────────────────────────────────────

MENU = f"""
{C.BOLD}  ── Read ──────────────────────────────────────{C.RESET}
  [1]  FC01  Read Coils
  [2]  FC02  Read Discrete Inputs
  [3]  FC03  Read Holding Registers
  [4]  FC04  Read Input Registers

{C.BOLD}  ── Write ─────────────────────────────────────{C.RESET}
  [5]  FC05  Write Single Coil
  [6]  FC06  Write Single Register
  [7]  FC15  Write Multiple Coils
  [8]  FC16  Write Multiple Registers
  [9]  FC22  Mask Write Register
  [10] FC23  Read/Write Multiple Registers

{C.BOLD}  ── Tools ─────────────────────────────────────{C.RESET}
  [11]       Poll Registers (continuous)
  [12]       Change Slave ID
  [0]        Quit
"""

def prompt_int(msg, default=0):
    try:
        raw = input(f"  {C.YELLOW}{msg}{C.RESET} ").strip()
        return int(raw) if raw else default
    except (ValueError, EOFError):
        return default

def prompt_str(msg, default=""):
    try:
        raw = input(f"  {C.YELLOW}{msg}{C.RESET} ").strip()
        return raw if raw else default
    except EOFError:
        return default

def prompt_list_int(msg):
    raw = prompt_str(msg)
    try:
        return [int(x.strip()) for x in raw.split(",") if x.strip()]
    except ValueError:
        err("Expected comma-separated integers, e.g. 100,200,300"); return []

def prompt_list_bool(msg):
    raw = prompt_str(msg)
    result = []
    for x in raw.split(","):
        x = x.strip().lower()
        result.append(x in ("1","true","on","yes"))
    return result


def interactive_loop(client, slave_id):
    slave = slave_id
    while True:
        print(MENU)
        choice = prompt_int("Select [0-12]:", 99)

        if choice == 0:
            info("Disconnecting…")
            client.close()
            sys.exit(0)

        elif choice == 1:
            addr  = prompt_int("Start address [0]:", 0)
            count = prompt_int("Count [1]:", 1) or 1
            read_coils(client, slave, addr, count)

        elif choice == 2:
            addr  = prompt_int("Start address [0]:", 0)
            count = prompt_int("Count [1]:", 1) or 1
            read_discrete_inputs(client, slave, addr, count)

        elif choice == 3:
            addr  = prompt_int("Start address [0]:", 0)
            count = prompt_int("Count [1]:", 1) or 1
            read_holding_registers(client, slave, addr, count)

        elif choice == 4:
            addr  = prompt_int("Start address [0]:", 0)
            count = prompt_int("Count [1]:", 1) or 1
            read_input_registers(client, slave, addr, count)

        elif choice == 5:
            addr = prompt_int("Coil address [0]:", 0)
            val  = prompt_str("Value (on/off/1/0) [off]:").lower()
            write_single_coil(client, slave, addr, val in ("on","1","true","yes"))

        elif choice == 6:
            addr = prompt_int("Register address [0]:", 0)
            val  = prompt_int("Value 0–65535 [0]:", 0)
            write_single_register(client, slave, addr, val)

        elif choice == 7:
            addr   = prompt_int("Start address [0]:", 0)
            values = prompt_list_bool("Values comma-sep (1,0,1,0) [1]:") or [True]
            write_multiple_coils(client, slave, addr, values)

        elif choice == 8:
            addr   = prompt_int("Start address [0]:", 0)
            values = prompt_list_int("Values comma-sep (100,200,300) [0]:") or [0]
            write_multiple_registers(client, slave, addr, values)

        elif choice == 9:
            addr     = prompt_int("Register address [0]:", 0)
            and_mask = int(prompt_str("AND mask hex (e.g. 0xF2F2) [0xFFFF]:") or "0xFFFF", 16)
            or_mask  = int(prompt_str("OR  mask hex (e.g. 0x0025) [0x0000]:") or "0x0000", 16)
            mask_write_register(client, slave, addr, and_mask, or_mask)

        elif choice == 10:
            r_addr  = prompt_int("Read start addr [0]:", 0)
            r_count = prompt_int("Read count [1]:", 1) or 1
            w_addr  = prompt_int("Write start addr [0]:", 0)
            w_vals  = prompt_list_int("Write values comma-sep [0]:") or [0]
            read_write_multiple_registers(client, slave, r_addr, r_count, w_addr, w_vals)

        elif choice == 11:
            fc       = prompt_int("FC (1/2/3/4) [3]:", 3) or 3
            addr     = prompt_int("Start address [0]:", 0)
            count    = prompt_int("Count [1]:", 1) or 1
            interval = float(prompt_str("Poll interval seconds [1.0]:") or "1.0")
            poll_registers(client, slave, addr, count, interval, fc)

        elif choice == 12:
            slave = prompt_int(f"New slave ID (current={slave}) [1]:", 1) or 1
            ok(f"Slave ID → {slave}")

        else:
            warn("Unknown option.")

        print()


# ─────────────────────────────────────────────────────────────────────────────
# Argument parser
# ─────────────────────────────────────────────────────────────────────────────

def build_parser():
    p = argparse.ArgumentParser(
        description="Modbus Master Simulator (pymodbus 3.6.x)",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    sub = p.add_subparsers(dest="mode", required=True)

    tcp = sub.add_parser("tcp", help="Modbus TCP")
    tcp.add_argument("--host",    default="127.0.0.1")
    tcp.add_argument("--port",    type=int, default=502)
    tcp.add_argument("--slave",   type=int, default=1)
    tcp.add_argument("--timeout", type=float, default=3)

    for name in ("rtu", "ascii"):
        sp = sub.add_parser(name, help=f"Modbus {name.upper()} serial")
        sp.add_argument("--port",     required=True)
        sp.add_argument("--baudrate", type=int, default=9600,
                        choices=[1200,2400,4800,9600,19200,38400,57600,115200])
        sp.add_argument("--parity",   default="N", choices=["N","E","O"])
        sp.add_argument("--stopbits", type=int, default=1, choices=[1,2])
        sp.add_argument("--bytesize", type=int, default=8, choices=[7,8])
        sp.add_argument("--slave",    type=int, default=1)
        sp.add_argument("--timeout",  type=float, default=3)

    return p


def main():
    print(BANNER)
    if len(sys.argv) == 1:
        print(f"  {C.YELLOW}Usage examples:{C.RESET}")
        print(f"    python modbus_master.py tcp --host 192.168.1.10 --port 502 --slave 1")
        print(f"    python modbus_master.py rtu --port COM3 --baudrate 9600")
        print(f"    python modbus_master.py ascii --port /dev/ttyUSB0 --baudrate 19200\n")
        build_parser().print_help()
        sys.exit(0)

    args = build_parser().parse_args()

    try:
        if args.mode == "tcp":
            client = connect_tcp(args.host, args.port, args.timeout)
        else:
            client = connect_serial(args.port, args.baudrate, args.parity,
                                    args.stopbits, args.bytesize,
                                    args.timeout, args.mode)
    except ConnectionException as e:
        err(str(e))
        sys.exit(1)

    interactive_loop(client, args.slave)


if __name__ == "__main__":
    main()