"""
mqtt_transport.py
------------------
Primary config-delivery mode for production/field deployment where the
board is behind a USB/cellular dongle (dynamic IP, likely CGNAT — not
directly reachable from the tool's PC). The board already maintains an
outbound MQTT connection to HiveMQ for telemetry publish, so we reuse
that same broker/connection as the config-push channel: no inbound
connectivity to the board is required at all.

Topic layout (per device, keyed by device_id so the board's IP is
irrelevant):

    amset/<device_id>/config/set   <- tool publishes registers.csv content
    amset/<device_id>/config/ack   <- board publishes back after applying

Firmware side (new work required — see FIRMWARE_NOTES.md in this repo):
subscribe to config/set, atomically replace registers.csv, reload points
in the running process, then publish a short JSON ack.

Requires: paho-mqtt
"""

from __future__ import annotations
import json
import ssl
import threading
import time
from typing import Optional

import paho.mqtt.client as mqtt

from core.transport import Transport, TransportResult


# Keep the old name around as an alias so existing call sites that
# reference ``MQTTResult`` directly keep working during migration.
MQTTResult = TransportResult


class MQTTTransport(Transport):
    def __init__(self, broker: str, port: int, username: str, password: str,
                 use_tls: bool = True):
        self.broker = broker
        self.port = port
        self.username = username
        self.password = password
        self.use_tls = use_tls

    def _make_client(self, client_id_suffix: str):
        """Returns (client, connected_event).  The event is set by an
        on_connect callback once the MQTT CONNACK is received — callers
        must wait on it before publishing."""
        connected_event = threading.Event()
        conn_error = {}

        client = mqtt.Client(client_id=f"smart_rtu_tool_{client_id_suffix}")
        client.username_pw_set(self.username, self.password)
        if self.use_tls:
            client.tls_set(cert_reqs=ssl.CERT_REQUIRED)

        def on_connect(cli, userdata, flags, rc):
            if rc == 0:
                connected_event.set()
            else:
                conn_error["rc"] = rc
                connected_event.set()  # unblock the waiter so it can read the error

        client.on_connect = on_connect
        client._conn_error = conn_error
        return client, connected_event

    def _connect_and_wait(self, client, connected_event, timeout=10):
        """Connects and blocks until CONNACK or timeout.  Returns an
        error string on failure, None on success."""
        client.connect(self.broker, self.port, keepalive=max(timeout, 20))
        client.loop_start()
        if not connected_event.wait(timeout=timeout):
            client.loop_stop()
            return "Timed out waiting for broker CONNACK — check broker address, port, and TLS settings."
        rc = client._conn_error.get("rc")
        if rc:
            client.loop_stop()
            reasons = {1: "bad protocol", 2: "client-id rejected",
                       3: "broker unavailable", 4: "bad credentials",
                       5: "not authorised"}
            return f"Broker refused connection: {reasons.get(rc, f'rc={rc}')}."
        return None

    def push_file(self, local_path: str, remote_path: str,
                  **kwargs) -> TransportResult:
        """MQTT transport cannot upload files to the device."""
        return TransportResult(
            False,
            "MQTT transport cannot upload files to the device. "
            "Use SSH or Serial mode to upload certificates, or place "
            "them on the board manually."
        )

    def test_connection(self) -> TransportResult:
        try:
            client, evt = self._make_client("test")
            err = self._connect_and_wait(client, evt, timeout=10)
            if err:
                return TransportResult(False, err)
            client.loop_stop()
            client.disconnect()
            return TransportResult(True, "Broker connection OK")
        except Exception as e:
            return TransportResult(False, f"Broker connection failed: {e}")

    def push_combined_config(self, device_id: str, csv_content: str, json_content: str,
                              wait_ack_seconds: int = 15, **kwargs) -> TransportResult:
        """
        Publishes the ONE combined config (Wi-Fi + Device + Modbus Map
        Sizing + Registers) to the device over MQTT. Both the CSV and
        JSON versions are sent — the CSV on .../config/set (what the
        current firmware watches, see FIRMWARE_NOTES.md) and the JSON
        on .../config/set.json (for firmware that prefers to parse
        JSON instead). Only the CSV publish is waited on for an ack.
        """
        set_topic = f"amset/{device_id}/config/set"
        ack_topic = f"amset/{device_id}/config/ack"
        json_topic = f"amset/{device_id}/config/set.json"

        result = self.push_text(csv_content, set_topic, ack_topic=ack_topic,
                                wait_ack_seconds=wait_ack_seconds)
        try:
            client, evt = self._make_client("push_json")
            err = self._connect_and_wait(client, evt, timeout=10)
            if not err:
                info = client.publish(json_topic, json_content, qos=1, retain=True)
                info.wait_for_publish(timeout=10)
                client.loop_stop()
                client.disconnect()
        except Exception:
            pass  # JSON copy is best-effort; the CSV push result above is authoritative
        return result

    def push_text(self, content: str, set_topic: str, ack_topic: str = "",
                  wait_ack_seconds: int = 15, **kwargs) -> TransportResult:
        """
        Publishes content (retained, QoS 1) to set_topic, then waits
        briefly for an ack on ack_topic. Used for registers.csv as well
        as the standalone Wi-Fi / device-config Send buttons — just with
        different topic names. Returns ok=True only if an ack was
        actually received — publish succeeding does NOT mean the board
        applied it, since the board may be offline right now (dongle
        dropout).
        """
        result = {"ack": None}

        def on_message(client, userdata, msg):
            try:
                result["ack"] = json.loads(msg.payload.decode())
            except Exception:
                result["ack"] = {"raw": msg.payload.decode(errors="replace")}

        try:
            client, evt = self._make_client("push")
            client.on_message = on_message
            err = self._connect_and_wait(client, evt, timeout=10)
            if err:
                return TransportResult(False, err)

            if ack_topic:
                client.subscribe(ack_topic, qos=1)
                time.sleep(0.5)  # let subscribe register before publishing

            info = client.publish(set_topic, content, qos=1, retain=True)
            info.wait_for_publish(timeout=10)

            if ack_topic:
                waited = 0.0
                while result["ack"] is None and waited < wait_ack_seconds:
                    time.sleep(0.3)
                    waited += 0.3

            client.loop_stop()
            client.disconnect()

            if result["ack"] is not None:
                return TransportResult(
                    True,
                    f"Applied — {result['ack'].get('points_loaded', 'ack received')}",
                    ack_payload=result["ack"],
                )
            return TransportResult(
                False,
                "Published, but no acknowledgment received within "
                f"{wait_ack_seconds}s — device may be offline. "
                "Config was retained on the broker and will apply "
                "automatically when the device reconnects.",
            )
        except Exception as e:
            return TransportResult(False, f"MQTT push failed: {e}")

    def read_text(self, topic: str, timeout: float = 5.0,
                  **kwargs) -> TransportResult:
        """
        Subscribes to `topic` and waits for the retained message the
        broker delivers on subscribe. This is best-effort: it shows the
        last value the tool itself last pushed to the broker (retained),
        not necessarily a confirmed current read from the device unless
        the device publishes its own state to this exact topic.
        """
        result = {"payload": None}

        def on_message(client, userdata, msg):
            if result["payload"] is None:
                result["payload"] = msg.payload.decode(errors="replace")

        try:
            client, evt = self._make_client("read")
            client.on_message = on_message
            err = self._connect_and_wait(client, evt, timeout=10)
            if err:
                return TransportResult(False, err)
            client.subscribe(topic, qos=1)

            waited = 0.0
            while result["payload"] is None and waited < timeout:
                time.sleep(0.2)
                waited += 0.2

            client.loop_stop()
            client.disconnect()

            if result["payload"] is not None:
                return TransportResult(True, result["payload"])
            return TransportResult(False, f"No retained message found on {topic} — nothing has been sent yet, or the broker/topic doesn't have a retained value.")
        except Exception as e:
            return TransportResult(False, f"MQTT read failed: {e}")

    def delete_remote(self, topic: str, **kwargs) -> TransportResult:
        """Clears a retained message by publishing an empty payload with
        retain=True — the standard MQTT way to erase a retained value.
        This erases data ON THE BROKER (and, once the device re-reads,
        effectively on the device), not local files on this PC."""
        try:
            client, evt = self._make_client("clear")
            err = self._connect_and_wait(client, evt, timeout=10)
            if err:
                return TransportResult(False, err)
            info = client.publish(topic, payload="", qos=1, retain=True)
            info.wait_for_publish(timeout=10)
            client.loop_stop()
            client.disconnect()
            return TransportResult(True, f"Cleared retained value on {topic}")
        except Exception as e:
            return TransportResult(False, f"Clear failed: {e}")
