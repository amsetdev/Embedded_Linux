"""AWS IoT access for the HIL network tests: which variables they need, and an
observer client (its own test certificate) that records what the gateway publishes
and can publish commands to it.

Variables (CI: Protected; locally: tests/hil/run_local.sh reads them from outside the repo):
  HIL_AWS_ENDPOINT   xxxx-ats.iot.<region>.amazonaws.com
  HIL_DEVICE_ID      gateway device_id = AWS IoT thing name / client ID
  HIL_AWS_CA         File: Amazon Root CA 1
  HIL_DEVICE_CERT, HIL_DEVICE_KEY       File: the gateway's TEST certificate / key
  HIL_OBSERVER_CERT, HIL_OBSERVER_KEY   File: the observer's certificate / key
Without them the network tests are skipped, never failed.
"""

import json
import os
import queue
import ssl
import threading
import time

import pytest

NETWORK_VARS = ["HIL_AWS_ENDPOINT", "HIL_DEVICE_ID", "HIL_AWS_CA", "HIL_DEVICE_CERT", "HIL_DEVICE_KEY",
                "HIL_OBSERVER_CERT", "HIL_OBSERVER_KEY"]


def missing_network_vars():
    return [v for v in NETWORK_VARS if not os.environ.get(v)]


def skip_without_network():
    missing = missing_network_vars()
    if missing:
        pytest.skip(f"AWS IoT variables not set: {', '.join(missing)} (DOCS/CI_CD_GUIDE.md §5.9)")


def endpoint():
    host = os.environ["HIL_AWS_ENDPOINT"].strip()
    for prefix in ("mqtts://", "ssl://", "tls://"):
        if host.startswith(prefix):
            host = host[len(prefix):]
    return host.split(":")[0].rstrip("/")


class Observer:
    """MQTT client in AWS IoT (default) or another TLS broker (local_broker.py).
    Always close() it (a leftover keeps reconnecting)."""

    def __init__(self, topics, host=None, port=8883, ca=None, cert=None, key=None, client_id=None):
        import paho.mqtt.client as mqtt

        self.messages = queue.Queue()
        self.connected = threading.Event()
        client_id = client_id or f"{os.environ['HIL_DEVICE_ID']}-observer"
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        self.client.tls_set(ca_certs=ca or os.environ["HIL_AWS_CA"], certfile=cert or os.environ["HIL_OBSERVER_CERT"],
                            keyfile=key or os.environ["HIL_OBSERVER_KEY"], cert_reqs=ssl.CERT_REQUIRED)
        self.client.on_connect = lambda c, u, f, rc, p=None: (
            [c.subscribe(t, qos=1) for t in topics], self.connected.set())
        self.client.on_message = lambda c, u, m: self.messages.put((m.topic, m.payload, time.time()))
        self.client.connect(host or endpoint(), port, keepalive=30)
        self.client.loop_start()
        if not self.connected.wait(20):
            self.close()
            raise AssertionError(f"observer could not connect to {host or 'AWS IoT'} (endpoint, CA or observer certificate/policy)")
        time.sleep(1)   # let the subscriptions settle

    def wait_for(self, topic, predicate=lambda payload: True, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                t, payload, _ = self.messages.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                break
            if t == topic and predicate(payload):
                return payload
        raise AssertionError(f"nothing matching on {topic} within {timeout}s")

    def wait_json(self, topic, predicate=lambda d: True, timeout=60):
        def ok(payload):
            try:
                return predicate(json.loads(payload))
            except ValueError:
                return False
        return json.loads(self.wait_for(topic, ok, timeout))

    def publish(self, topic, payload, retain=False):
        info = self.client.publish(topic, payload, qos=1, retain=retain)
        info.wait_for_publish(timeout=10)

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()
