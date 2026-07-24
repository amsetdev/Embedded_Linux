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
import time
from dataclasses import dataclass
from typing import Optional

import paho.mqtt.client as mqtt


@dataclass
class MQTTResult:
    ok: bool
    message: str
    ack_payload: Optional[dict] = None


class MQTTTransport:
    def __init__(self, broker: str, port: int, username: str, password: str,
                 use_tls: bool = True):
        self.broker = broker
        self.port = port
        self.username = username
        self.password = password
        self.use_tls = use_tls

    def _make_client(self, client_id_suffix: str) -> mqtt.Client:
        client = mqtt.Client(client_id=f"smart_rtu_tool_{client_id_suffix}")
        client.username_pw_set(self.username, self.password)
        if self.use_tls:
            client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
        return client

    def test_connection(self) -> MQTTResult:
        try:
            client = self._make_client("test")
            client.connect(self.broker, self.port, keepalive=10)
            client.loop_start()
            time.sleep(1.5)
            connected = client.is_connected()
            client.loop_stop()
            client.disconnect()
            if connected:
                return MQTTResult(True, "Broker connection OK")
            return MQTTResult(False, "Could not confirm broker connection")
        except Exception as e:
            return MQTTResult(False, f"Broker connection failed: {e}")

    def push_combined_config(self, device_id: str, csv_content: str, json_content: str,
                              wait_ack_seconds: int = 15) -> MQTTResult:
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

        result = self.push_text(set_topic, ack_topic, csv_content, wait_ack_seconds)
        try:
            client = self._make_client("push_json")
            client.connect(self.broker, self.port, keepalive=10)
            client.loop_start()
            info = client.publish(json_topic, json_content, qos=1, retain=True)
            info.wait_for_publish(timeout=10)
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass  # JSON copy is best-effort; the CSV push result above is authoritative
        return result

    def push_text(self, set_topic: str, ack_topic: str, content: str,
                  wait_ack_seconds: int = 15) -> MQTTResult:
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
            client = self._make_client("push")
            client.on_message = on_message
            client.connect(self.broker, self.port, keepalive=20)
            client.loop_start()

            client.subscribe(ack_topic, qos=1)
            time.sleep(0.5)  # let subscribe register before publishing

            info = client.publish(set_topic, content, qos=1, retain=True)
            info.wait_for_publish(timeout=10)

            waited = 0.0
            while result["ack"] is None and waited < wait_ack_seconds:
                time.sleep(0.3)
                waited += 0.3

            client.loop_stop()
            client.disconnect()

            if result["ack"] is not None:
                return MQTTResult(
                    True,
                    f"Applied — {result['ack'].get('points_loaded', 'ack received')}",
                    ack_payload=result["ack"],
                )
            return MQTTResult(
                False,
                "Published, but no acknowledgment received within "
                f"{wait_ack_seconds}s — device may be offline. "
                "Config was retained on the broker and will apply "
                "automatically when the device reconnects.",
            )
        except Exception as e:
            return MQTTResult(False, f"MQTT push failed: {e}")

    def read_retained(self, topic: str, timeout: float = 5.0) -> MQTTResult:
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
            client = self._make_client("read")
            client.on_message = on_message
            client.connect(self.broker, self.port, keepalive=10)
            client.loop_start()
            client.subscribe(topic, qos=1)

            waited = 0.0
            while result["payload"] is None and waited < timeout:
                time.sleep(0.2)
                waited += 0.2

            client.loop_stop()
            client.disconnect()

            if result["payload"] is not None:
                return MQTTResult(True, result["payload"])
            return MQTTResult(False, f"No retained message found on {topic} — nothing has been sent yet, or the broker/topic doesn't have a retained value.")
        except Exception as e:
            return MQTTResult(False, f"MQTT read failed: {e}")

    def clear_retained(self, topic: str) -> MQTTResult:
        """Clears a retained message by publishing an empty payload with
        retain=True — the standard MQTT way to erase a retained value.
        This erases data ON THE BROKER (and, once the device re-reads,
        effectively on the device), not local files on this PC."""
        try:
            client = self._make_client("clear")
            client.connect(self.broker, self.port, keepalive=10)
            client.loop_start()
            info = client.publish(topic, payload="", qos=1, retain=True)
            info.wait_for_publish(timeout=10)
            client.loop_stop()
            client.disconnect()
            return MQTTResult(True, f"Cleared retained value on {topic}")
        except Exception as e:
            return MQTTResult(False, f"Clear failed: {e}")
