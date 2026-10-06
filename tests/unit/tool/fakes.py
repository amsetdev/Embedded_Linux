"""Fake board, console, broker and clock for the tool's transport tests.

Each fake implements only what the tool uses, but implements it like the real
thing: the SSH fake runs `mv`/`rm`/`tail` against an in-memory filesystem, the
serial fake is a login shell on a TTY (it echoes what you type, like a real
console, and understands the heredoc/`mv`/`rm`/`echo`/`cat` lines the tool
types), the MQTT fake is a broker with retained messages.
"""

import shlex
import threading
from types import SimpleNamespace


class FakeClock:
    """Replaces the `time` module inside a transport: sleep() advances time() instantly."""

    def __init__(self):
        self.now = 1_000_000.0
        self.slept = 0.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds
        self.slept += seconds


# --------------------------------------------------------------------------- SSH

class FakeBoardFS:
    """The board's filesystem and command log, shared by every SSH connection."""

    def __init__(self, files=None):
        self.files = dict(files or {})
        self.commands = []
        self.connections = []        # kwargs of every connect()
        self.refuse = None           # exception to raise on connect()

    def run(self, cmd):
        """Execute one shell command; return (stdout, stderr)."""
        self.commands.append(cmd)
        argv = shlex.split(cmd.split(";")[0].split("&&")[0])
        if argv[0] == "mv":
            if len(argv) != 3:
                return "", f"mv: bad arguments {argv[1:]}"
            src, dst = argv[1], argv[2]
            if src not in self.files:
                return "", f"mv: cannot stat '{src}': No such file or directory"
            self.files[dst] = self.files.pop(src)
            return "", ""
        if argv[0] == "rm":
            for p in argv[2:] if argv[1] == "-f" else argv[1:]:
                self.files.pop(p, None)
            return "", ""
        if argv[0] == "tail":
            return self.files.get(argv[-1], b"").decode(), ""
        return "", ""     # pkill/nohup etc.: accepted, recorded in self.commands


class _Stream:
    def __init__(self, text):
        self._data = text.encode()
        self.channel = SimpleNamespace(recv_exit_status=lambda: 0)

    def read(self):
        return self._data


class _RemoteFile:
    def __init__(self, fs, path, mode):
        self.fs, self.path, self.mode = fs, path, mode
        if "r" in mode and path not in fs.files:
            raise FileNotFoundError(path)
        self.buf = b""

    def write(self, data):
        self.buf += data.encode() if isinstance(data, str) else data

    def read(self):
        return self.fs.files[self.path]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if "w" in self.mode:
            self.fs.files[self.path] = self.buf


class FakeSSHClient:
    """Stands in for paramiko.SSHClient; construct via ssh_client_factory(fs)."""

    def __init__(self, fs):
        self.fs = fs

    def set_missing_host_key_policy(self, policy):
        pass

    def connect(self, host, **kwargs):
        if self.fs.refuse:
            raise self.fs.refuse
        self.fs.connections.append({"host": host, **kwargs})

    def open_sftp(self):
        fs = self.fs
        return SimpleNamespace(open=lambda path, mode="r": _RemoteFile(fs, path, mode), close=lambda: None)

    def exec_command(self, cmd):
        out, err = self.fs.run(cmd)
        return None, _Stream(out), _Stream(err)

    def close(self):
        pass


def ssh_client_factory(fs):
    return lambda: FakeSSHClient(fs)


# ------------------------------------------------------------------------ serial

class FakeConsole:
    """A Linux login shell on a serial TTY, as seen through pyserial.

    echo=True is how a real console behaves: every character typed is echoed
    back. Line endings: "\\r\\n" and "\\n" both end a line (no icrnl modelling).
    """

    def __init__(self, files=None, echo=True, login=("root", None), fail_mv=False):
        self.files = dict(files or {})
        self.echo = echo
        self.user, self.password = login
        self.fail_mv = fail_mv
        self.state = "login" if self.user else "shell"
        self.out = b""
        self.typed = b""
        self._line = b""
        self._heredoc = None          # (path, terminator, lines)
        self.is_open = True

    # pyserial API -------------------------------------------------------
    @property
    def in_waiting(self):
        return len(self.out)

    def read(self, n=1):
        data, self.out = self.out[:n], self.out[n:]
        return data

    def write(self, data):
        self.typed += data
        for b in data:
            ch = bytes([b])
            if ch == b"\r":
                continue
            if self.echo and not (self.state == "password"):
                self.out += b"\r\n" if ch == b"\n" else ch
            if ch == b"\n":
                line, self._line = self._line.decode(), b""
                self._handle(line)
            else:
                self._line += ch
        return len(data)

    def close(self):
        self.is_open = False

    # shell --------------------------------------------------------------
    def _print(self, text):
        self.out += text.encode().replace(b"\n", b"\r\n")

    def _handle(self, line):
        if self.state == "login":
            if line == "":
                self._print("\nboard login: ")
            elif line == self.user:
                if self.password:
                    self.state = "password"
                    self._print("Password: ")
                else:
                    self.state = "shell"
                    self._print("root@board:~# ")
            return
        if self.state == "password":
            self.state = "shell" if line == self.password else "login"
            self._print("\nroot@board:~# " if self.state == "shell" else "\nLogin incorrect\nboard login: ")
            return
        if self._heredoc:
            path, term, lines = self._heredoc
            if line == term:
                self.files[path] = "".join(l + "\n" for l in lines)
                self._heredoc = None
            else:
                lines.append(line)
            return
        for part in line.split(";"):
            self._command(part.strip())
        self._print("root@board:~# ")

    def _command(self, cmd):
        if not cmd:
            return
        chain = [c.strip() for c in cmd.split("&&")]
        for c in chain:
            argv = shlex.split(c.replace("<<", " << "))
            if argv[:1] == ["cat"] and ">" in argv and "<<" in argv:
                self._heredoc = (argv[argv.index(">") + 1], argv[argv.index("<<") + 1], [])
                return
            if not self._run(argv):
                return

    def _run(self, argv):
        """Run one simple command; return False if it failed (stops an && chain)."""
        name = argv[0]
        if name == "echo":
            self._print(" ".join(argv[1:]) + "\n")
            return True
        if name == "cat":
            path = argv[1]
            if path not in self.files:
                self._print(f"cat: {path}: No such file or directory\n")
                return False
            self._print(self.files[path])
            return True
        if name == "mv":
            if self.fail_mv or argv[1] not in self.files:
                self._print(f"mv: cannot move '{argv[1]}': Permission denied\n")
                return False
            self.files[argv[2]] = self.files.pop(argv[1])
            return True
        if name == "rm":
            for p in argv[1:]:
                if not p.startswith("-"):
                    self.files.pop(p, None)
            return True
        self._print(f"sh: {name}: not found\n")
        return False


# -------------------------------------------------------------------------- MQTT

class FakeBroker:
    """An MQTT broker: retained messages, subscriptions, optional auto-reply."""

    def __init__(self):
        self.retained = {}
        self.published = []           # (client_id, topic, payload, qos, retain)
        self.clients = []
        self.connack_rc = 0           # 0 = accepted, 5 = not authorised, ...
        self.never_connack = False
        self.responders = {}          # topic -> callable(payload) -> (reply_topic, reply_payload)

    def client_factory(self):
        broker = self

        def make(client_id="", **kwargs):
            c = FakeMQTTClient(broker, client_id)
            broker.clients.append(c)
            return c
        return make

    def deliver(self, topic, payload):
        for c in self.clients:
            if topic in c.subscriptions and c.on_message:
                c.on_message(c, None, SimpleNamespace(topic=topic, payload=payload))


class FakeMQTTClient:
    def __init__(self, broker, client_id):
        self.broker = broker
        self.client_id = client_id
        self.on_connect = None
        self.on_message = None
        self.subscriptions = []
        self.tls = None
        self.credentials = None
        self.connected_to = None
        self.loop_running = False

    def username_pw_set(self, username, password):
        self.credentials = (username, password)

    def tls_set(self, **kwargs):
        self.tls = kwargs

    def connect(self, host, port, keepalive=60):
        self.connected_to = (host, port)

    def loop_start(self):
        self.loop_running = True
        if self.on_connect and not self.broker.never_connack:
            self.on_connect(self, None, {}, self.broker.connack_rc)

    def loop_stop(self):
        self.loop_running = False

    def disconnect(self):
        pass

    def subscribe(self, topic, qos=0):
        self.subscriptions.append(topic)
        if topic in self.broker.retained and self.on_message:
            self.on_message(self, None, SimpleNamespace(topic=topic, payload=self.broker.retained[topic]))

    def publish(self, topic, payload=None, qos=0, retain=False):
        data = payload.encode() if isinstance(payload, str) else (payload or b"")
        self.broker.published.append((self.client_id, topic, data, qos, retain))
        if retain:
            if data:
                self.broker.retained[topic] = data
            else:
                self.broker.retained.pop(topic, None)
        if topic in self.broker.responders:
            reply_topic, reply = self.broker.responders[topic](data)
            self.broker.deliver(reply_topic, reply)
        return SimpleNamespace(wait_for_publish=lambda timeout=None: True, is_published=lambda: True)


# -------------------------------------------------------------------- transport

class FakeTransport:
    """A Transport that records calls; results come from `results[method]`."""

    def __init__(self, results=None):
        from core.transport import TransportResult
        self.calls = []
        self._ok = TransportResult(True, "ok")
        self.results = results or {}
        self.lock = threading.Lock()

    def _result(self, name, *args, **kwargs):
        with self.lock:
            self.calls.append((name, args, kwargs))
        r = self.results.get(name, self._ok)
        return r(*args, **kwargs) if callable(r) else r

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *a, **k: self._result(name, *a, **k)

    def names(self):
        return [c[0] for c in self.calls]
