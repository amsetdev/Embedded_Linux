"""A local MQTT broker for the HIL tests, so MQTT features are tested without AWS
and without opening a port on this PC:

  board: gateway --TLS--> 127.0.0.1:BOARD_PORT ==SSH reverse forward==> this PC: mosquitto

mosquitto (apt package "mosquitto") listens on 127.0.0.1 only and requires a client
certificate. Every run makes a throw-away CA, a server certificate for "localhost",
a device certificate (installed on the board) and a client certificate for the test.

    with LocalBroker(board) as broker:
        broker.install_device_certs()           # /etc/gateway/certs/hil-local-*.{crt,key}
        cfg["mqtt"] = broker.mqtt_section(device_id, telemetry_topic)
        observer = broker.observer([topic, ...])
"""

import datetime
import select
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from aws_link import Observer
from board import CERTS

BOARD_PORT = 18883


def _name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def _cert(subject, key, issuer_name, issuer_key, ca=False, san=None):
    now = datetime.datetime.now(datetime.timezone.utc)
    b = (x509.CertificateBuilder().subject_name(_name(subject)).issuer_name(issuer_name)
         .public_key(key.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(hours=1)).not_valid_after(now + datetime.timedelta(days=2))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if san:
        b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    return b.sign(issuer_key, hashes.SHA256())


def make_certs(d):
    """CA + server (localhost) + device + client certificates in directory d; returns their paths."""
    import ipaddress

    d = Path(d)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca = _cert("hil-local-ca", ca_key, _name("hil-local-ca"), ca_key, ca=True)
    paths = {"ca": d / "ca.crt"}
    paths["ca"].write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    for role, cn, san in (("server", "localhost", [x509.DNSName("localhost"),
                                                    x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]),
                          ("device", "hil-device", None), ("client", "hil-client", None)):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = _cert(cn, key, ca.subject, ca_key, san=san)
        paths[f"{role}_crt"] = d / f"{role}.crt"
        paths[f"{role}_key"] = d / f"{role}.key"
        paths[f"{role}_crt"].write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        paths[f"{role}_key"].write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                                           serialization.PrivateFormat.TraditionalOpenSSL,
                                                           serialization.NoEncryption()))
        paths[f"{role}_key"].chmod(0o600)
    return paths


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class LocalBroker:
    def __init__(self, board):
        if not shutil.which("mosquitto"):
            pytest.skip("mosquitto not installed on the test PC/container (apt-get install mosquitto)")
        self.board = board
        self.dir = Path(tempfile.mkdtemp(prefix="hil-broker-"))
        self.certs = make_certs(self.dir)
        # mosquitto started as root drops to user "mosquitto": it must read the
        # (throw-away, per-run) server files
        self.dir.chmod(0o755)
        for f in ("ca", "server_crt", "server_key"):
            self.certs[f].chmod(0o644)
        self.port = _free_port()
        conf = self.dir / "mosquitto.conf"
        conf.write_text(
            f"listener {self.port} 127.0.0.1\n"
            f"cafile {self.certs['ca']}\ncertfile {self.certs['server_crt']}\nkeyfile {self.certs['server_key']}\n"
            "require_certificate true\nuse_identity_as_username true\nallow_anonymous false\n"
            "persistence false\nlog_type error\nlog_type warning\nlog_type notice\n")
        self.log = open(self.dir / "mosquitto.log", "w")
        self.proc = subprocess.Popen(["mosquitto", "-c", str(conf)], stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while True:
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.5).close()
                break
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"local mosquitto did not start: {(self.dir / 'mosquitto.log').read_text()}")
                time.sleep(0.2)
        self.transport = board.ssh.get_transport()
        self.transport.request_port_forward("127.0.0.1", BOARD_PORT, handler=self._on_channel)

    # ------------------------------------------------------------ forwarding
    def _on_channel(self, chan, origin, server):
        threading.Thread(target=self._pump, args=(chan,), daemon=True).start()

    def _pump(self, chan):
        try:
            sock = socket.create_connection(("127.0.0.1", self.port), 5)
        except OSError:
            chan.close()
            return
        try:
            while True:
                r, _, _ = select.select([sock, chan], [], [], 1.0)
                if sock in r:
                    data = sock.recv(65536)
                    if not data:
                        break
                    chan.sendall(data)
                if chan in r:
                    data = chan.recv(65536)
                    if not data:
                        break
                    sock.sendall(data)
        except OSError:
            pass
        finally:
            sock.close()
            chan.close()

    # ------------------------------------------------------------ for the tests
    def install_device_certs(self):
        for src, name in (("ca", "hil-local-ca.crt"), ("device_crt", "hil-local-device.crt"),
                          ("device_key", "hil-local-device.key")):
            self.board.put(self.certs[src], f"{CERTS}/{name}", mode=0o600)

    def mqtt_section(self, device_id, topic):
        return {"broker": "localhost", "port": BOARD_PORT, "client_id": device_id,
                "ca_cert": f"{CERTS}/hil-local-ca.crt", "device_cert": f"{CERTS}/hil-local-device.crt",
                "private_key": f"{CERTS}/hil-local-device.key", "topic": topic}

    def observer(self, topics):
        return Observer(topics, host="127.0.0.1", port=self.port, ca=str(self.certs["ca"]),
                        cert=str(self.certs["client_crt"]), key=str(self.certs["client_key"]),
                        client_id="hil-observer")

    def close(self):
        try:
            self.transport.cancel_port_forward("127.0.0.1", BOARD_PORT)
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
