"""
ssh_transport.py
-----------------
Direct SSH/SFTP delivery mode. Only usable when the tool's PC can reach
the board's IP directly (LAN, or a dongle that happens to expose a
reachable address). This is the FALLBACK path — MQTT is primary for
field/production deployments behind NAT dongles (see mqtt_transport.py).

Requires: paramiko
"""

from __future__ import annotations
import io
from dataclasses import dataclass
from typing import Optional

import paramiko


REMOTE_CONFIG_CSV_PATH = "/home/root/edb_c/linking/smart_rtu_config.csv"
REMOTE_CONFIG_JSON_PATH = "/home/root/edb_c/linking/smart_rtu_config.json"
REMOTE_APP_DIR = "/home/root/edb_c/linking"
REMOTE_APP_BIN = "./main"


@dataclass
class SSHResult:
    ok: bool
    message: str


class SSHTransport:
    def __init__(self, host: str, username: str, password: str,
                 port: int = 22, timeout: int = 10):
        self.host = host
        self.username = username
        self.password = password
        self.port = port
        self.timeout = timeout

    def _connect(self) -> paramiko.SSHClient:
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(
            self.host,
            port=self.port,
            username=self.username,
            password=self.password,
            timeout=self.timeout,
        )
        return ssh

    def test_connection(self) -> SSHResult:
        try:
            ssh = self._connect()
            ssh.close()
            return SSHResult(True, "Connected OK")
        except Exception as e:
            return SSHResult(False, f"Connection failed: {e}")

    def push_text(self, content: str, remote_path: str) -> SSHResult:
        """Writes content directly to the board over SFTP, no local temp
        file needed on the tool's PC. Used for registers.csv, and also for
        the standalone Wi-Fi / device-config Send buttons."""
        try:
            ssh = self._connect()
            sftp = ssh.open_sftp()
            # write to a .tmp path first, then rename on the remote side
            # so a dropped connection mid-transfer can't leave a half
            # written file that the firmware picks up.
            tmp_path = remote_path + ".tmp"
            with sftp.open(tmp_path, "w") as remote_file:
                remote_file.write(content)
            sftp.close()

            # atomic rename on the remote filesystem
            stdin, stdout, stderr = ssh.exec_command(
                f"mv {tmp_path} {remote_path}"
            )
            err = stderr.read().decode().strip()
            ssh.close()
            if err:
                return SSHResult(False, f"Remote rename failed: {err}")
            return SSHResult(True, f"{remote_path.rsplit('/', 1)[-1]} pushed to {remote_path}")
        except Exception as e:
            return SSHResult(False, f"Push failed: {e}")

    def push_csv(self, csv_content: str,
                 remote_path: str = REMOTE_CONFIG_CSV_PATH) -> SSHResult:
        return self.push_text(csv_content, remote_path)

    def push_combined_config(self, csv_content: str, json_content: str) -> SSHResult:
        """
        Writes BOTH the .csv and .json versions of the one combined
        config (Wi-Fi + Device + Modbus Map Sizing + Registers) to the
        device in a single call. Whichever format the firmware actually
        reads, it's there — no more juggling three separate files.
        """
        r1 = self.push_text(csv_content, REMOTE_CONFIG_CSV_PATH)
        if not r1.ok:
            return r1
        r2 = self.push_text(json_content, REMOTE_CONFIG_JSON_PATH)
        if not r2.ok:
            return r2
        return SSHResult(True, f"smart_rtu_config.csv and smart_rtu_config.json "
                                f"both pushed to {REMOTE_APP_DIR}/")

    def read_text(self, remote_path: str) -> SSHResult:
        """Reads a remote file's content back — used by the Read Wi-Fi /
        Read Config buttons. message contains the file content on
        success."""
        try:
            ssh = self._connect()
            sftp = ssh.open_sftp()
            try:
                with sftp.open(remote_path, "r") as remote_file:
                    content = remote_file.read().decode(errors="replace")
            except FileNotFoundError:
                sftp.close(); ssh.close()
                return SSHResult(False, f"No file found on device at {remote_path}")
            sftp.close()
            ssh.close()
            return SSHResult(True, content)
        except Exception as e:
            return SSHResult(False, f"Read failed: {e}")

    def delete_remote(self, remote_path: str) -> SSHResult:
        """Removes a file on the device — used by the Erase Wi-Fi / Erase
        Config buttons on the Device Config page. This erases data ON THE
        DEVICE, not local files on this PC."""
        try:
            ssh = self._connect()
            stdin, stdout, stderr = ssh.exec_command(f"rm -f {remote_path}")
            stdout.channel.recv_exit_status()
            err = stderr.read().decode().strip()
            ssh.close()
            if err:
                return SSHResult(False, f"Remote delete failed: {err}")
            return SSHResult(True, f"Erased {remote_path} on device")
        except Exception as e:
            return SSHResult(False, f"Delete failed: {e}")

    def restart_app(self) -> SSHResult:
        """Kills and relaunches ./main so it reloads registers.csv.
        Prefer reload_config() below if your firmware supports the
        MQTT/file-watch reload-in-place path instead of a hard restart."""
        try:
            ssh = self._connect()
            cmd = (
                f"pkill -f {REMOTE_APP_BIN} ; sleep 1 ; "
                f"cd {REMOTE_APP_DIR} && "
                f"nohup {REMOTE_APP_BIN} > /home/root/edb_c/linking/main.log 2>&1 & "
                f"disown"
            )
            stdin, stdout, stderr = ssh.exec_command(cmd)
            stdout.channel.recv_exit_status()  # wait for command to be issued
            ssh.close()
            return SSHResult(True, "Restart command issued")
        except Exception as e:
            return SSHResult(False, f"Restart failed: {e}")

    def read_remote_log_tail(self, lines: int = 30) -> SSHResult:
        try:
            ssh = self._connect()
            stdin, stdout, stderr = ssh.exec_command(
                f"tail -n {lines} {REMOTE_APP_DIR}/main.log"
            )
            out = stdout.read().decode(errors="replace")
            ssh.close()
            return SSHResult(True, out)
        except Exception as e:
            return SSHResult(False, f"Log read failed: {e}")
