"""
serial_transport.py
---------------------
Direct USB/serial delivery mode — the third option alongside MQTT and
SSH. Used when the technician's PC is physically plugged into the
board via USB (the same /dev/ttyACM0-style CDC-ACM port class your
Modbus RTU adapter also shows up as, OR a dedicated console/debug USB
port on the DK2 if one is wired to a Linux getty).

How it works: this does NOT use the Modbus RTU wire itself (that stays
dedicated to talking to field slaves). Instead it opens a serial
connection to the board's Linux **console/login shell** (a getty
exposed over USB-CDC, common on ST dev boards) and drives it like a
scripted terminal session: log in, then use `cat > file << EOF` style
shell redirection to write registers.csv directly, no SSH/network
needed at all. This is the fallback of last resort when there is no
IP connectivity whatsoever on site (no dongle signal, no LAN).

Requires: pyserial
"""

from __future__ import annotations
import time
from dataclasses import dataclass
from typing import Optional

import serial


REMOTE_CONFIG_CSV_PATH = "/home/root/edb_c/linking/smart_rtu_config.csv"
REMOTE_CONFIG_JSON_PATH = "/home/root/edb_c/linking/smart_rtu_config.json"


@dataclass
class SerialResult:
    ok: bool
    message: str


class SerialTransport:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 3.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout

    @staticmethod
    def list_ports():
        """Returns a list of (device, description) tuples for the port
        picker dropdown — mirrors what pymodslave/Arduino IDE-style
        tools show."""
        import serial.tools.list_ports as lp
        return [(p.device, p.description) for p in lp.comports()]

    def _open(self) -> serial.Serial:
        return serial.Serial(self.port, self.baud, timeout=self.timeout)

    def test_connection(self) -> SerialResult:
        try:
            ser = self._open()
            ser.write(b"\r\n")
            time.sleep(0.3)
            data = ser.read(ser.in_waiting or 64)
            ser.close()
            return SerialResult(True, f"Port {self.port} opened OK "
                                 f"({len(data)} bytes seen on wake)")
        except Exception as e:
            return SerialResult(False, f"Could not open {self.port}: {e}")

    def _login(self, ser: serial.Serial, username: str, password: Optional[str]):
        ser.write(b"\r\n")
        time.sleep(0.5)
        banner = ser.read(ser.in_waiting or 256).decode(errors="replace")
        if "login:" in banner.lower():
            ser.write((username + "\r\n").encode())
            time.sleep(0.5)
            resp = ser.read(ser.in_waiting or 256).decode(errors="replace")
            if "password:" in resp.lower() and password:
                ser.write((password + "\r\n").encode())
                time.sleep(0.5)
                ser.read(ser.in_waiting or 256)

    def push_text(self, content: str, remote_path: str,
                  username: str = "root", password: Optional[str] = None) -> SerialResult:
        """
        Logs into the board's serial console (if a login prompt is
        detected) and writes content via a heredoc, same technique used
        manually with `cat > file << 'EOF' ... EOF` earlier over SSH —
        just typed over the serial line instead of a network socket.
        """
        try:
            ser = self._open()
            self._login(ser, username, password)

            marker = "__RTU_TOOL_DONE__"
            cmd = f"cat > {remote_path}.tmp << 'EOF'\n{content}EOF\n"
            cmd += f"mv {remote_path}.tmp {remote_path} && echo {marker}\n"
            ser.write(cmd.encode())

            deadline = time.time() + 10
            buf = b""
            while time.time() < deadline:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    buf += chunk
                    if marker.encode() in buf:
                        break
                else:
                    time.sleep(0.1)
            ser.close()

            if marker.encode() in buf:
                return SerialResult(True, f"{remote_path.rsplit('/', 1)[-1]} written via serial console to {remote_path}")
            return SerialResult(
                False,
                "No completion marker seen — check the port is actually "
                "connected to the board's console (not the RS485/Modbus "
                "adapter) and that the login prompt/credentials matched."
            )
        except Exception as e:
            return SerialResult(False, f"Serial push failed: {e}")

    def push_csv(self, csv_content: str, username: str = "root",
                 password: Optional[str] = None,
                 remote_path: str = REMOTE_CONFIG_CSV_PATH) -> SerialResult:
        return self.push_text(csv_content, remote_path, username, password)

    def push_combined_config(self, csv_content: str, json_content: str,
                              username: str = "root",
                              password: Optional[str] = None) -> SerialResult:
        """Writes BOTH the .csv and .json versions of the one combined
        config to the device over the console, in a single call."""
        r1 = self.push_text(csv_content, REMOTE_CONFIG_CSV_PATH, username, password)
        if not r1.ok:
            return r1
        r2 = self.push_text(json_content, REMOTE_CONFIG_JSON_PATH, username, password)
        if not r2.ok:
            return r2
        return SerialResult(True, "smart_rtu_config.csv and smart_rtu_config.json "
                                   "both written via serial console")

    def read_text(self, remote_path: str, username: str = "root",
                   password: Optional[str] = None) -> SerialResult:
        """Reads a remote file back over the console (cat + markers) —
        used by Read Wi-Fi / Read Config."""
        try:
            ser = self._open()
            self._login(ser, username, password)

            start_marker = "__RTU_TOOL_START__"
            end_marker = "__RTU_TOOL_END__"
            cmd = f"echo {start_marker}; cat {remote_path}; echo {end_marker}\n"
            ser.write(cmd.encode())

            deadline = time.time() + 10
            buf = b""
            while time.time() < deadline:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    buf += chunk
                    if end_marker.encode() in buf:
                        break
                else:
                    time.sleep(0.1)
            ser.close()

            text = buf.decode(errors="replace")
            if start_marker not in text or end_marker not in text:
                return SerialResult(False, "No response seen — check console connection/login.")
            content = text.split(start_marker, 1)[1].split(end_marker, 1)[0].strip("\r\n")
            if "No such file" in content:
                return SerialResult(False, f"No file found on device at {remote_path}")
            return SerialResult(True, content)
        except Exception as e:
            return SerialResult(False, f"Serial read failed: {e}")

    def delete_remote(self, remote_path: str, username: str = "root",
                        password: Optional[str] = None) -> SerialResult:
        """Removes a file on the device over the console — used by Erase
        Wi-Fi / Erase Config. Erases data ON THE DEVICE, not this PC."""
        try:
            ser = self._open()
            self._login(ser, username, password)
            marker = "__RTU_TOOL_DONE__"
            ser.write(f"rm -f {remote_path} && echo {marker}\n".encode())

            deadline = time.time() + 10
            buf = b""
            while time.time() < deadline:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    buf += chunk
                    if marker.encode() in buf:
                        break
                else:
                    time.sleep(0.1)
            ser.close()
            if marker.encode() in buf:
                return SerialResult(True, f"Erased {remote_path} on device")
            return SerialResult(False, "No completion marker seen — check console connection.")
        except Exception as e:
            return SerialResult(False, f"Serial delete failed: {e}")
