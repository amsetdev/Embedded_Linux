"""Hardware-in-the-loop session on the CI DK2 (DOCS/CI_CD_GUIDE.md §5.9).

Once per session:
  1. back up the board's own configuration (/etc/gateway) and buffered payloads
     (/var/lib/gateway/storage) to /var/lib/gateway-hil-backup,
  2. install the CI build with the production installer (install.sh --no-migrate),
  3. run the tests with test configurations (Wi-Fi off, simulated Modbus slaves),
  4. restore the board's configuration and payloads and restart the service.

Variables:
  HIL_BOARD_HOST     board IP (no host: the whole suite is skipped)
  HIL_SSH_KEY        private key file for root@board (CI: File variable), or
  HIL_SSH_PASSWORD   password (local runs only)
  HIL_BUILD_DIR      build-firmware artifacts: gateway-<ver>.tar.gz, build_info.json (default build)
  HIL_SIM_HOST       this PC's IP as the board sees it (Modbus TCP slave), default 192.168.1.2
  HIL_RTU_PORT       RS485 adapter wired to the board's RS485 (RTU tests skip without it)
  AWS variables: see aws_link.py
"""

import json
import os
import shlex
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from board import BINARY, CERTS, CONFIG, STORAGE, Board  # noqa: E402
from sim import Slaves  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
HOST = os.environ.get("HIL_BOARD_HOST", "")
BUILD = Path(os.environ.get("HIL_BUILD_DIR", REPO / "build"))
SIM_HOST = os.environ.get("HIL_SIM_HOST", "192.168.1.2")
RTU_PORT = os.environ.get("HIL_RTU_PORT", "")
TCP_PORT = int(os.environ.get("HIL_TCP_PORT", "5020"))
DEVICE_ID = os.environ.get("HIL_DEVICE_ID") or "HIL-DK2"
BACKUP = "/var/lib/gateway-hil-backup"
INTERVAL = 5

# Registers of the test configuration and the value the simulated slave holds.
# 32-bit values high word first; the discrete input's coil twin holds the opposite.
RTU_REGISTERS = [
    ({"label": "S1_HOLD_U16", "address": 10, "type": "holding", "data_type": "uint16", "slave_id": 1}, 12345),
    ({"label": "S1_HOLD_I32", "address": 20, "type": "holding", "data_type": "int32", "slave_id": 1}, -123456),
    ({"label": "S1_HOLD_F32", "address": 30, "type": "holding", "data_type": "float32", "slave_id": 1}, 3.25),
    ({"label": "S1_IN_U16", "address": 40, "type": "input", "data_type": "uint16", "slave_id": 1}, 4321),
    ({"label": "S1_IN_F32", "address": 50, "type": "input", "data_type": "float32", "slave_id": 1}, -7.5),
    ({"label": "S1_COIL", "address": 60, "type": "coil", "data_type": "uint16", "slave_id": 1}, 1),
    ({"label": "S1_DISC", "address": 70, "type": "discrete", "data_type": "uint16", "slave_id": 1}, 1),
    ({"label": "S2_HOLD_U16", "address": 10, "type": "holding", "data_type": "uint16", "slave_id": 2}, 222),
    ({"label": "S2_HOLD_I32_BIG", "address": 12, "type": "holding", "data_type": "int32", "slave_id": 2}, 16777217),
]
RTU_EXPECTED = {reg["label"]: value for reg, value in RTU_REGISTERS}


def pytest_collection_modifyitems(config, items):
    if not HOST:
        skip = pytest.mark.skip(reason="HIL_BOARD_HOST not set (no CI board)")
        for item in items:
            item.add_marker(skip)


def base_config(registers=(), tcp=False, mqtt=None):
    """A test configuration: Wi-Fi off (the board's network is never touched)."""
    return {
        "device": {"device_id": DEVICE_ID, "slave_id": 1, "baud": 9600, "parity": "None",
                   "stop_bits": 1, "interval_sec": INTERVAL},
        "wifi": {"ssid": "", "password": "", "country": "IN", "enable": 0},
        "modbus_tcp": {"enable": 1 if tcp else 0, "ip": SIM_HOST if tcp else "", "port": TCP_PORT, "slave_id": 1},
        "mqtt": mqtt or {"broker": "", "port": 8883, "client_id": DEVICE_ID,
                         "ca_cert": f"{CERTS}/hil-ca.crt", "device_cert": f"{CERTS}/hil-device.crt",
                         "private_key": f"{CERTS}/hil-device.key", "topic": f"hil/{DEVICE_ID}/telemetry"},
        "registers": [dict(r) for r in registers],
    }


class Gateway:
    """The installed gateway on the board, driven by the tests."""

    def __init__(self, board, build_info):
        self.b = board
        self.build_info = build_info

    def apply_config(self, cfg, wait=r"\[DATA\] Loaded \d+ registers|No register configuration"):
        """Write the config, restart the service, wait until it has read it. Returns the journal mark."""
        self.b.put(json.dumps(cfg, indent=2), CONFIG, mode=0o600)
        mark = self.b.mark()
        self.b.service("restart")
        self.b.wait_log(mark, r"\[MAIN\] gateway ", timeout=30)
        if wait:
            self.b.wait_log(mark, wait, timeout=60)
        return mark

    def clear_storage(self):
        self.b.run(f"find {STORAGE} -maxdepth 1 -name '*.txt' -exec rm -f {{}} +")

    def stored(self):
        return sorted(n for n in self.b.listdir(STORAGE) if n.endswith(".txt"))

    def wait_stored(self, after=(), count=1, timeout=90):
        """Wait for `count` new payload files (names not in `after`); returns their parsed JSON."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            new = [n for n in self.stored() if n not in set(after)]
            if len(new) >= count:
                return [(n, json.loads(self.b.read(f"{STORAGE}/{n}"))) for n in new[:count]]
            time.sleep(2)
        raise AssertionError(f"fewer than {count} new payload file(s) in {STORAGE} within {timeout}s")


@pytest.fixture(scope="session")
def build_info():
    path = BUILD / "build_info.json"
    if not path.exists():
        pytest.fail(f"{path} not found: run with the build-firmware artifacts (HIL_BUILD_DIR)")
    return json.loads(path.read_text())


@pytest.fixture(scope="session")
def board(build_info):
    b = Board(HOST, key_file=os.environ.get("HIL_SSH_KEY") or None,
              password=os.environ.get("HIL_SSH_PASSWORD") or None)
    q = shlex.quote
    session_mark = b.mark()
    b.session_mark = session_mark
    # 1. Back up what belongs to the board (kept from an aborted earlier run if present).
    b.run(f"if [ ! -d {BACKUP} ]; then mkdir -p {BACKUP}/storage && "
          f"([ -d /etc/gateway ] && cp -a /etc/gateway {BACKUP}/etc-gateway || true) && "
          f"([ -d {STORAGE} ] && find {STORAGE} -maxdepth 1 -name '*.txt' -exec mv {{}} {BACKUP}/storage/ \\; || true); fi")
    try:
        # 2. Install the CI build exactly like production.
        version = build_info["app_version"]
        pkg = BUILD / f"gateway-{version}.tar.gz"
        if not pkg.exists():
            pytest.fail(f"{pkg} not found (make package / build-firmware artifacts)")
        b.put(pkg, "/tmp/hil-gateway.tar.gz")
        out = b.run("rm -rf /tmp/hil-gw && mkdir /tmp/hil-gw && tar -C /tmp/hil-gw -xzf /tmp/hil-gateway.tar.gz "
                    "&& sh /tmp/hil-gw/install.sh --no-migrate", timeout=300).out
        print(out)
        b.install_log = out
        b.expected_restarts = 0
        yield b
    finally:
        # Whole gateway journal of the session: CI artifact reports/hil-journal.log.
        try:
            (REPO / "reports").mkdir(exist_ok=True)
            (REPO / "reports" / "hil-journal.log").write_text(b.since(session_mark))
        except Exception as e:      # never hide the test result behind a log problem
            print(f"could not save the journal: {e}")
        # 4. Put the board back: its config, its buffered payloads, its service.
        b.run(f"systemctl stop gateway; find {STORAGE} -maxdepth 1 -name '*.txt' -exec rm -f {{}} +; "
              f"if [ -d {BACKUP}/etc-gateway ]; then rm -rf /etc/gateway && cp -a {BACKUP}/etc-gateway /etc/gateway; fi; "
              f"find {BACKUP}/storage -maxdepth 1 -name '*.txt' -exec mv {{}} {STORAGE}/ \\; ; "
              f"rm -rf {q(BACKUP)} /tmp/hil-gw /tmp/hil-gateway.tar.gz; systemctl start gateway", check=False)
        b.close()


@pytest.fixture(scope="session")
def gateway(board, build_info):
    return Gateway(board, build_info)


@pytest.fixture(scope="session")
def slaves():
    """Simulated RTU slaves 1 and 2 (on HIL_RTU_PORT) and a Modbus TCP slave (port HIL_TCP_PORT)."""
    s = Slaves()
    for reg, value in RTU_REGISTERS:
        s.load(reg, value)
    s.set_tcp_holding(0, [1000 + i for i in range(100)])
    s.start(rtu_port=RTU_PORT or None,
            rtu_serial=dict(baudrate=9600, parity="N", stopbits=1, bytesize=8, timeout=0.05),
            tcp_port=TCP_PORT)
    yield s
    s.stop()


def rtu_available():
    return bool(RTU_PORT) and os.path.exists(RTU_PORT)
