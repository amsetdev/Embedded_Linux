"""The CI board over SSH: commands, files, gateway.service and its journal.

    b = Board("192.168.1.26", key_file="~/.ssh/hil_ed25519")
    mark = b.mark()                       # position in the gateway journal
    b.service("restart")
    b.wait_log(mark, r"Loaded \\d+ registers", timeout=60)

Never prints secrets: commands and their output are only shown when a check
fails, and callers must not pass secrets on command lines (use put()).
"""

import io
import re
import shlex
import time
from dataclasses import dataclass

import paramiko

SERVICE = "gateway"
CONFIG = "/etc/gateway/smart_rtu_config.json"
CERTS = "/etc/gateway/certs"
DATA = "/var/lib/gateway"
STORAGE = f"{DATA}/storage"
BINARY = "/opt/gateway/bin/gateway"


@dataclass
class Result:
    rc: int
    out: str
    err: str


class BoardError(AssertionError):
    pass


def load_private_key(value):
    """The CI SSH key from HIL_SSH_KEY: a file path (GitLab File variable) or the key text
    itself. Tolerates what copy-pasting into the GitLab UI does to a key: CRLF line endings,
    surrounding whitespace, a missing final newline. On failure the message describes the
    content's shape, never the content."""
    if not value:
        return None
    text = value
    if not value.lstrip().startswith("-----BEGIN"):
        with open(value, encoding="utf-8", errors="replace") as f:
            text = f.read()
    has_cr = "\r" in text
    lines = [l.strip() for l in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    lines = [l for l in lines if l]
    clean = "\n".join(lines) + "\n"
    errors = []
    for cls in (paramiko.Ed25519Key, paramiko.ECDSAKey, paramiko.RSAKey):
        try:
            return cls.from_private_key(io.StringIO(clean))
        except (paramiko.SSHException, ValueError) as e:
            errors.append(f"{cls.__name__}: {e}")
    if len(lines) == 1 and ("/" in lines[0] or lines[0].startswith("~")) and "BEGIN" not in lines[0]:
        raise BoardError(
            "HIL_SSH_KEY contains a file path, not the key: a GitLab File variable's VALUE must be "
            "the key text itself (the whole content of ~/.config/embedded_linux/hil/id_ed25519, from "
            "-----BEGIN to -----END). DOCS/HIL_SETUP.md §2, or let tests/hil/setup_runner.sh set it.")
    begin = lines[0] if lines and lines[0].startswith("-----BEGIN") else "(no -----BEGIN line)"
    end = lines[-1] if lines and lines[-1].startswith("-----END") else "(no -----END line)"
    raise BoardError(
        "HIL_SSH_KEY is not a usable private key: "
        f"{len(lines)} non-empty lines, first {begin!r}, last {end!r}, CR characters: {has_cr}. "
        "Paste the WHOLE file ~/.config/embedded_linux/hil/id_ed25519 into a variable of type File "
        f"(DOCS/HIL_SETUP.md §2). [{'; '.join(errors)}]")


class Board:
    def __init__(self, host, user="root", key_file=None, password=None, port=22):
        self.host = host
        self.ssh = paramiko.SSHClient()
        self.ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())   # dedicated CI board on the LAN
        self.ssh.connect(host, port=port, username=user, pkey=load_private_key(key_file),
                         password=password, timeout=15, allow_agent=False, look_for_keys=False)
        self.expected_restarts = 0

    def close(self):
        self.ssh.close()

    # -------------------------------------------------------------- commands
    def run(self, cmd, check=True, timeout=120):
        stdin, stdout, stderr = self.ssh.exec_command(cmd, timeout=timeout)
        out = stdout.read().decode(errors="replace")
        err = stderr.read().decode(errors="replace")
        rc = stdout.channel.recv_exit_status()
        if check and rc != 0:
            raise BoardError(f"board command failed (exit {rc}): {cmd}\n{out[-2000:]}\n{err[-2000:]}")
        return Result(rc, out, err)

    def put(self, data, remote, mode=0o644):
        """Write bytes/str (or a local file path given as pathlib.Path) to the board."""
        if hasattr(data, "read_bytes"):
            data = data.read_bytes()
        if isinstance(data, str):
            data = data.encode()
        sftp = self.ssh.open_sftp()
        try:
            tmp = remote + ".hil-tmp"
            sftp.putfo(io.BytesIO(data), tmp)
            sftp.chmod(tmp, mode)
            sftp.posix_rename(tmp, remote)
        finally:
            sftp.close()

    def read(self, remote):
        sftp = self.ssh.open_sftp()
        try:
            with sftp.open(remote, "r") as f:
                return f.read().decode(errors="replace")
        finally:
            sftp.close()

    def exists(self, path):
        return self.run(f"test -e {shlex.quote(path)}", check=False).rc == 0

    def listdir(self, path):
        r = self.run(f"ls -1 {shlex.quote(path)}", check=False)
        return [l for l in r.out.splitlines() if l] if r.rc == 0 else []

    # --------------------------------------------------------------- service
    def service(self, action):
        """systemctl <action> gateway; a restart is an expected restart for test_99."""
        self.run(f"systemctl {action} {SERVICE}")

    def prop(self, name):
        return self.run(f"systemctl show {SERVICE} -p {name} --value").out.strip()

    def main_pid(self):
        return int(self.prop("MainPID") or 0)

    def is_active(self):
        return self.run(f"systemctl is-active --quiet {SERVICE}", check=False).rc == 0

    # --------------------------------------------------------------- journal
    def mark(self):
        """Cursor of the newest gateway journal entry (lines after it are 'new')."""
        out = self.run(f"journalctl -u {SERVICE} -n 1 --show-cursor -o cat --no-pager").out
        m = re.search(r"^-- cursor: (.+)$", out, re.M)
        return m.group(1).strip() if m else ""

    def since(self, mark):
        """Everything the gateway logged after `mark` (all lines if mark is empty)."""
        after = f"--after-cursor {shlex.quote(mark)}" if mark else ""
        return self.run(f"journalctl -u {SERVICE} {after} -o cat --no-pager").out

    def wait_log(self, mark, pattern, timeout=60, poll=1.0):
        """Wait until a line after `mark` matches; returns the match. Fails with the log tail."""
        rx = re.compile(pattern, re.M)
        deadline = time.monotonic() + timeout
        text = ""
        while time.monotonic() < deadline:
            text = self.since(mark)
            m = rx.search(text)
            if m:
                return m
            time.sleep(poll)
        tail = "\n".join(text.splitlines()[-30:])
        raise BoardError(f"timeout ({timeout}s) waiting for /{pattern}/ in the gateway journal; last lines:\n{tail}")
