"""
main_window.py
---------------
Smart RTU Tool — 3-page wizard UI.

Page 1 — Device Config:  device name, slave id, baud, poll interval, Wi-Fi
Page 2 — Data Transmission: Excel/manual entry (one shared table), Validate,
          Save Project, Export CSV, connection mode tabs (MQTT / SSH /
          Serial-USB), Test Connection, Refresh, Push, Retry Queue
Page 3 — Status: live log + last result, big status indicator
"""

from __future__ import annotations
import sys

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QTableWidget, QTableWidgetItem, QPushButton, QLabel, QLineEdit,
    QComboBox, QSpinBox, QTabWidget, QGroupBox, QFormLayout, QMessageBox,
    QFileDialog, QTextEdit, QCheckBox, QHeaderView, QProgressDialog,
    QAbstractItemView, QStackedWidget, QFrame
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal

import os

from core import (
    REMOTE_CONFIG_CSV_PATH, REMOTE_CONFIG_JSON_PATH,
    REMOTE_CA_CERT_PATH, REMOTE_DEVICE_CERT_PATH, REMOTE_PRIVATE_KEY_PATH,
)
from core.register_model import DeviceConfig, RegisterPoint
from core.transport import TransportResult
from core.ssh_transport import SSHTransport
from core.mqtt_transport import MQTTTransport
from core.serial_transport import SerialTransport
from core.excel_import import import_excel
from core import offline_queue, credentials


COLUMNS = ["Slave ID", "Label", "Address", "Type", "Data Type"]

REG_TYPES = ["holding", "input", "coil", "discrete"]
DATA_TYPES = ["uint16", "float32", "int32"]

APP_STYLESHEET = """
QMainWindow, QWidget { background-color: #f4f6f9; font-family: "Segoe UI", "Ubuntu", sans-serif; font-size: 10pt; }
QGroupBox { background-color: #ffffff; border: 1px solid #d8dee6; border-radius: 8px; margin-top: 14px; padding: 10px; font-weight: 600; color: #2c3e50; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #34495e; }
QPushButton { background-color: #ffffff; border: 1px solid #c3cad4; border-radius: 6px; padding: 6px 14px; color: #2c3e50; }
QPushButton:hover { background-color: #eaf1fb; border-color: #4a90d9; }
QPushButton:pressed { background-color: #d6e6fa; }
QPushButton:disabled { color: #a5adb8; background-color: #f0f2f5; }
QPushButton#primaryButton { background-color: #2e7dd7; color: white; font-weight: 600; border: none; padding: 9px 20px; }
QPushButton#primaryButton:hover { background-color: #2568b3; }
QPushButton#navButton { background-color: #2c3e50; color: white; font-weight: 600; border: none; padding: 9px 22px; }
QPushButton#navButton:hover { background-color: #1c2b3a; }
QPushButton#navButton:disabled { background-color: #aab4bf; }
QLineEdit, QComboBox, QSpinBox { border: 1px solid #c3cad4; border-radius: 5px; padding: 4px 6px; background: white; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #4a90d9; }
QTableWidget { background: white; border: 1px solid #d8dee6; border-radius: 6px; gridline-color: #ecf0f3; alternate-background-color: #f7f9fb; selection-background-color: #cfe4fb; }
QHeaderView::section { background-color: #eef2f7; color: #2c3e50; padding: 6px; border: none; border-right: 1px solid #d8dee6; font-weight: 600; }
QTabWidget::pane { border: 1px solid #d8dee6; border-radius: 6px; background: white; top: -1px; }
QTabBar::tab { background: #eef2f7; padding: 7px 18px; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 2px; color: #56606b; }
QTabBar::tab:selected { background: white; color: #2e7dd7; font-weight: 600; }
QTextEdit#logView { background: #1e2530; color: #d7e0ea; border-radius: 6px; font-family: Consolas, "Courier New", monospace; font-size: 9pt; padding: 6px; }
QLabel#hintLabel { color: #8a92a0; font-style: italic; }
QLabel#pointCountLabel { color: #2e7dd7; font-weight: 600; }
QLabel#stepLabel { color: #8a92a0; font-weight: 600; letter-spacing: 1px; }
QFrame#statusPill { border-radius: 10px; padding: 14px; }
QFrame#topNavBar { background-color: #2c3e50; }
QPushButton#topNavButton { background-color: transparent; color: #b8c4d0; border: none; border-radius: 5px; padding: 9px 18px; font-weight: 600; }
QPushButton#topNavButton:hover { background-color: #3a4f63; color: white; }
QPushButton#topNavButton:checked { background-color: #2e7dd7; color: white; }
QPushButton#readButton { background-color: #f2a93c; color: #2c1e00; font-weight: 600; border: 1px solid #d99328; padding: 7px 16px; }
QPushButton#readButton:hover { background-color: #e0982e; }
QPushButton#sendButton { background-color: #2e9e5b; color: white; font-weight: 600; border: none; padding: 7px 16px; }
QPushButton#sendButton:hover { background-color: #257e49; }
QPushButton#eraseButton { background-color: #d9534f; color: white; font-weight: 600; border: none; padding: 7px 16px; }
QPushButton#eraseButton:hover { background-color: #c9302c; }
QLabel#miniStatusLabel { color: #4a5568; font-weight: 600; background: #eef2f7; border-radius: 5px; padding: 4px 10px; }
QGroupBox#summaryCard { border: 1px solid #cfe4fb; background-color: #f5faff; }
QLabel#summaryText { color: #22313f; font-size: 9pt; }
"""


def _make_transport(mode, conn_params):
    """Factory that builds the right Transport instance from the current
    connection-tab mode and parameter dict."""
    if mode == "mqtt":
        return MQTTTransport(
            broker=conn_params["broker"], port=conn_params["port"],
            username=conn_params["username"], password=conn_params["password"],
        )
    if mode == "ssh":
        return SSHTransport(
            host=conn_params["host"],
            username=conn_params["username"],
            password=conn_params["password"],
        )
    return SerialTransport(
        port=conn_params["port"],
        baud=conn_params.get("console_baud", 115200),
    )


class PushWorker(QThread):
    finished_ok = pyqtSignal(str)
    finished_fail = pyqtSignal(str)

    def __init__(self, mode, config, conn_params, cert_pairs=None):
        super().__init__()
        self.mode = mode
        self.config = config
        self.conn_params = conn_params
        self.cert_pairs = cert_pairs or []

    def _upload_certs(self, transport):
        """Upload local cert files to the board. Returns (ok, message)."""
        if not self.cert_pairs:
            return True, ""
        if self.mode == "mqtt":
            return True, ("Note: Certificates must be placed on the board "
                          "manually — MQTT mode cannot upload files.\n")
        uploaded = []
        for local_path, remote_path in self.cert_pairs:
            r = transport.push_file(
                local_path, remote_path,
                username=self.conn_params.get("username", "root"),
                password=self.conn_params.get("password"),
            )
            if not r.ok:
                return False, f"Failed to upload {os.path.basename(local_path)}: {r.message}"
            uploaded.append(os.path.basename(local_path))
        return True, f"Uploaded certificates: {', '.join(uploaded)}\n"

    def run(self):
        csv_content = self.config.to_full_config_csv_string()
        json_content = self.config.to_full_config_json()
        try:
            transport = _make_transport(self.mode, self.conn_params)

            # Upload certs first (if any local files were browsed)
            cert_ok, cert_msg = self._upload_certs(transport)
            if not cert_ok:
                self.finished_fail.emit(cert_msg)
                return

            if self.mode == "mqtt":
                result = transport.push_combined_config(
                    self.config.device_id, csv_content, json_content,
                    wait_ack_seconds=self.conn_params.get("ack_timeout", 15)
                )
                if result.ok:
                    self.finished_ok.emit(cert_msg + result.message)
                else:
                    offline_queue.enqueue(self.config.device_id, csv_content)
                    self.finished_fail.emit(cert_msg + result.message + "\n\nConfig queued locally for retry.")

            elif self.mode == "ssh":
                push_result = transport.push_combined_config(csv_content, json_content)
                if not push_result.ok:
                    self.finished_fail.emit(cert_msg + push_result.message)
                    return
                restart_result = transport.restart_app()
                if restart_result.ok:
                    self.finished_ok.emit(cert_msg + push_result.message + "\n" + restart_result.message)
                else:
                    self.finished_fail.emit(cert_msg + push_result.message + "\nRestart failed: " + restart_result.message)

            elif self.mode == "serial":
                result = transport.push_combined_config(
                    csv_content, json_content,
                    username=self.conn_params.get("username", "root"),
                    password=self.conn_params.get("password"),
                )
                if result.ok:
                    self.finished_ok.emit(cert_msg + result.message)
                else:
                    self.finished_fail.emit(cert_msg + result.message)
        except Exception as e:
            self.finished_fail.emit(f"Unexpected error: {e}")


class ConfigFieldWorker(QThread):
    """
    Runs Read / Send / Erase for the SAME combined file (Wi-Fi + Device
    Settings + Modbus Map Sizing + Registers, all together) that "Push
    Data to Device" uses — from the Device Config page's Read/Send/
    Erase Config buttons. Works across all three delivery methods
    (MQTT / SSH / Serial), reusing the same connection tab set up on
    Data Transmission.
    """
    finished = pyqtSignal(bool, str)

    def __init__(self, mode, action, config, conn_params, cert_pairs=None):
        super().__init__()
        self.mode = mode              # "mqtt" | "ssh" | "serial"
        self.action = action          # "send" | "read" | "erase"
        self.config = config
        self.conn_params = conn_params
        self.cert_pairs = cert_pairs or []

    def _csv_content(self):
        return self.config.to_full_config_csv_string()

    def _json_content(self):
        return self.config.to_full_config_json()

    def _mqtt_topics(self):
        device_id = self.config.device_id
        base = f"amset/{device_id}/config"
        return f"{base}/set", f"{base}/ack"

    def _upload_certs(self, transport):
        """Upload local cert files if this is a 'send' action. Returns (ok, message)."""
        if self.action != "send" or not self.cert_pairs:
            return True, ""
        if self.mode == "mqtt":
            return True, ("Note: Certificates must be placed on the board "
                          "manually — MQTT mode cannot upload files.\n")
        uploaded = []
        for local_path, remote_path in self.cert_pairs:
            r = transport.push_file(
                local_path, remote_path,
                username=self.conn_params.get("username", "root"),
                password=self.conn_params.get("password"),
            )
            if not r.ok:
                return False, f"Failed to upload {os.path.basename(local_path)}: {r.message}"
            uploaded.append(os.path.basename(local_path))
        return True, f"Uploaded certificates: {', '.join(uploaded)}\n"

    def run(self):
        label = "Settings (Wi-Fi + Device + Modbus + Registers)"
        try:
            transport = _make_transport(self.mode, self.conn_params)

            # Upload certs before sending config
            cert_ok, cert_msg = self._upload_certs(transport)
            if not cert_ok:
                self.finished.emit(False, f"{label} — Send: {cert_msg}")
                return

            if self.mode == "mqtt":
                set_topic, ack_topic = self._mqtt_topics()
                if self.action == "send":
                    r = transport.push_combined_config(
                        self.config.device_id, self._csv_content(), self._json_content(),
                        wait_ack_seconds=self.conn_params.get("ack_timeout", 15)
                    )
                elif self.action == "read":
                    r = transport.read_text(set_topic)
                else:  # erase
                    r = transport.delete_remote(set_topic)

            elif self.mode == "ssh":
                if self.action == "send":
                    r = transport.push_combined_config(self._csv_content(), self._json_content())
                elif self.action == "read":
                    r = transport.read_text(REMOTE_CONFIG_JSON_PATH)
                else:
                    r1 = transport.delete_remote(REMOTE_CONFIG_CSV_PATH)
                    r = transport.delete_remote(REMOTE_CONFIG_JSON_PATH) if r1.ok else r1

            else:  # serial
                username = self.conn_params.get("username", "root")
                password = self.conn_params.get("password")
                if self.action == "send":
                    r = transport.push_combined_config(self._csv_content(), self._json_content(),
                                                        username, password)
                elif self.action == "read":
                    r = transport.read_text(REMOTE_CONFIG_JSON_PATH, username, password)
                else:
                    r1 = transport.delete_remote(REMOTE_CONFIG_CSV_PATH, username, password)
                    r = transport.delete_remote(REMOTE_CONFIG_JSON_PATH, username, password) if r1.ok else r1

            prefix = f"{label} — {self.action.title()}: "
            self.finished.emit(r.ok, prefix + cert_msg + r.message)
        except Exception as e:
            self.finished.emit(False, f"{label} — {self.action.title()} failed: {e}")


class TestWorker(QThread):
    finished = pyqtSignal(bool, str)

    def __init__(self, mode, conn_params):
        super().__init__()
        self.mode = mode
        self.conn_params = conn_params

    def run(self):
        try:
            transport = _make_transport(self.mode, self.conn_params)
            r = transport.test_connection()
            self.finished.emit(r.ok, r.message)
        except Exception as e:
            self.finished.emit(False, str(e))


class RetryQueueWorker(QThread):
    """Drains the offline retry queue in a background thread so the UI
    stays responsive during MQTT publish+ack waits."""
    finished = pyqtSignal(list)  # list of result strings

    def __init__(self, conn_params):
        super().__init__()
        self.conn_params = conn_params

    def run(self):
        transport = MQTTTransport(
            self.conn_params["broker"], self.conn_params["port"],
            self.conn_params["username"], self.conn_params["password"],
        )
        results = []
        for item in offline_queue.list_queued():
            r = transport.push_text(
                item.csv_content,
                f"amset/{item.device_id}/config/set",
                ack_topic=f"amset/{item.device_id}/config/ack",
            )
            if r.ok:
                offline_queue.remove(item.filepath)
                results.append(f"{item.device_id}: delivered")
            else:
                results.append(f"{item.device_id}: still pending ({r.message})")
        self.finished.emit(results)


class CertUploadWorker(QThread):
    """Uploads local certificate files to the board before config push."""
    finished = pyqtSignal(bool, str)

    def __init__(self, mode, conn_params, cert_pairs):
        super().__init__()
        self.mode = mode
        self.conn_params = conn_params
        self.cert_pairs = cert_pairs  # list of (local_path, remote_path)

    def run(self):
        try:
            transport = _make_transport(self.mode, self.conn_params)
            uploaded = []
            for local_path, remote_path in self.cert_pairs:
                r = transport.push_file(local_path, remote_path,
                                        username=self.conn_params.get("username", "root"),
                                        password=self.conn_params.get("password"))
                if not r.ok:
                    self.finished.emit(False, f"Failed to upload {local_path}: {r.message}")
                    return
                uploaded.append(os.path.basename(local_path))
            self.finished.emit(True, f"Uploaded certificates: {', '.join(uploaded)}")
        except Exception as e:
            self.finished.emit(False, f"Certificate upload failed: {e}")


class DeviceConfigPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 1 OF 4 — DEVICE CONFIG")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        # --- Wi-Fi Settings | Device Settings — side by side ---------------
        top_row = QHBoxLayout()

        wifi_box = QGroupBox("Wi-Fi Settings")
        wifi_form = QFormLayout(wifi_box)
        self.wifi_ssid_edit = QLineEdit()
        self.wifi_ssid_edit.setToolTip("The Wi-Fi network name the board should connect to.")
        self.wifi_pass_edit = QLineEdit(); self.wifi_pass_edit.setEchoMode(QLineEdit.Password)
        self.wifi_pass_edit.setToolTip("The Wi-Fi network password.")
        self.wifi_show_chk = QCheckBox("Show password")
        self.wifi_show_chk.toggled.connect(
            lambda on: self.wifi_pass_edit.setEchoMode(QLineEdit.Normal if on else QLineEdit.Password)
        )
        wifi_form.addRow("SSID:", self.wifi_ssid_edit)
        wifi_form.addRow("Password:", self.wifi_pass_edit)
        wifi_form.addRow("", self.wifi_show_chk)
        note = QLabel("Leave blank if using a USB/cellular dongle or Ethernet.")
        note.setObjectName("hintLabel"); note.setWordWrap(True)
        wifi_form.addRow(note)
        top_row.addWidget(wifi_box)

        dev_box = QGroupBox("Device Settings")
        dev_form = QFormLayout(dev_box)
        self.device_id_edit = QLineEdit("AMSET-001")
        self.device_id_edit.setToolTip("A friendly name/ID for this device — also used as its MQTT topic key.")
        self.slave_id_spin = QSpinBox(); self.slave_id_spin.setRange(1, 247); self.slave_id_spin.setValue(1)
        self.slave_id_spin.setToolTip("The Modbus RTU slave address of this device (1-247).")
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200"])
        self.baud_combo.setCurrentText("9600")
        self.baud_combo.setToolTip("Modbus RTU field baud rate (RS485 wire speed).")
        self.interval_spin = QSpinBox(); self.interval_spin.setRange(1, 3600); self.interval_spin.setValue(30)
        self.interval_spin.setSuffix(" s")
        self.interval_spin.setToolTip("How often the device polls its Modbus registers.")
        self.parity_combo = QComboBox(); self.parity_combo.addItems(["None", "Even", "Odd"])
        self.parity_combo.setToolTip("Modbus RTU serial parity.")
        self.stop_bits_combo = QComboBox(); self.stop_bits_combo.addItems(["1", "2"])
        self.stop_bits_combo.setToolTip("Modbus RTU serial stop bits.")
        dev_form.addRow("Device Name / ID:", self.device_id_edit)
        dev_form.addRow("Default Slave ID:", self.slave_id_spin)
        dev_form.addRow("Baud Rate:", self.baud_combo)
        dev_form.addRow("Poll Interval:", self.interval_spin)
        dev_form.addRow("Parity:", self.parity_combo)
        dev_form.addRow("Stop Bits:", self.stop_bits_combo)
        tcp_box = QGroupBox("Modbus TCP")
        tcp_form = QFormLayout(tcp_box)
        self.tcp_enable_chk = QCheckBox("Enable")
        self.tcp_enable_chk.setToolTip("Enable/disable the Modbus TCP polling thread on the board.")
        self.tcp_ip_edit = QLineEdit()
        self.tcp_ip_edit.setPlaceholderText("e.g. 192.168.1.100")
        self.tcp_ip_edit.setToolTip("IP address of the Modbus TCP slave device to poll.")
        self.tcp_port_spin = QSpinBox(); self.tcp_port_spin.setRange(1, 65535); self.tcp_port_spin.setValue(502)
        self.tcp_port_spin.setToolTip("Modbus TCP port (standard: 502).")
        self.tcp_slave_spin = QSpinBox(); self.tcp_slave_spin.setRange(1, 247); self.tcp_slave_spin.setValue(1)
        self.tcp_slave_spin.setToolTip("Modbus TCP slave/unit ID.")
        tcp_form.addRow("", self.tcp_enable_chk)
        tcp_form.addRow("Slave IP:", self.tcp_ip_edit)
        tcp_form.addRow("Port:", self.tcp_port_spin)
        tcp_form.addRow("Slave ID:", self.tcp_slave_spin)

        # Grey out TCP fields when disabled
        self.tcp_enable_chk.toggled.connect(self.tcp_ip_edit.setEnabled)
        self.tcp_enable_chk.toggled.connect(self.tcp_port_spin.setEnabled)
        self.tcp_enable_chk.toggled.connect(self.tcp_slave_spin.setEnabled)
        self.tcp_ip_edit.setEnabled(False)
        self.tcp_port_spin.setEnabled(False)
        self.tcp_slave_spin.setEnabled(False)

        top_row.addWidget(dev_box)
        top_row.addWidget(tcp_box)

        layout.addLayout(top_row)

        # --- Action buttons ------------------------------------------------
        cfg_action_row = QHBoxLayout()
        self.cfg_status_label = QLabel("Settings: Ready")
        self.cfg_status_label.setObjectName("miniStatusLabel")
        cfg_action_row.addWidget(self.cfg_status_label)
        cfg_action_row.addStretch()
        self.cfg_read_btn = QPushButton("Read Settings from Device")
        self.cfg_read_btn.setObjectName("readButton")
        self.cfg_read_btn.setToolTip("Reads the Wi-Fi, Device, and Modbus settings currently stored on the device.")
        self.cfg_read_btn.clicked.connect(lambda: self._run_field_action("read"))
        self.cfg_send_btn = QPushButton("Send Settings to Device")
        self.cfg_send_btn.setObjectName("sendButton")
        self.cfg_send_btn.setToolTip("Sends everything above — Wi-Fi, Device Settings — to the device as one file.")
        self.cfg_send_btn.clicked.connect(lambda: self._run_field_action("send"))
        self.cfg_erase_btn = QPushButton("Erase Settings on Device")
        self.cfg_erase_btn.setObjectName("eraseButton")
        self.cfg_erase_btn.setToolTip("Erases the settings file stored ON THE DEVICE (not this PC).")
        self.cfg_erase_btn.clicked.connect(lambda: self._run_field_action("erase"))
        cfg_action_row.addWidget(self.cfg_read_btn)
        cfg_action_row.addWidget(self.cfg_send_btn)
        cfg_action_row.addWidget(self.cfg_erase_btn)
        layout.addLayout(cfg_action_row)

        layout.addStretch()

        nav = QHBoxLayout()
        nav.addStretch()
        next_btn = QPushButton("Next: MQTT Settings →")
        next_btn.setObjectName("navButton")
        next_btn.clicked.connect(lambda: self.mw.goto_page(1))
        nav.addWidget(next_btn)
        layout.addLayout(nav)

    def apply_to_config(self, config):
        config.device_id = self.device_id_edit.text().strip() or "AMSET-001"
        config.slave_id = self.slave_id_spin.value()
        config.baud = int(self.baud_combo.currentText())
        config.interval_sec = self.interval_spin.value()
        config.parity = self.parity_combo.currentText()
        config.stop_bits = int(self.stop_bits_combo.currentText())
        config.wifi_ssid = self.wifi_ssid_edit.text().strip()
        config.wifi_password = self.wifi_pass_edit.text()
        config.modbus_tcp_enable = 1 if self.tcp_enable_chk.isChecked() else 0
        config.modbus_tcp_ip = self.tcp_ip_edit.text().strip()
        config.modbus_tcp_port = self.tcp_port_spin.value()
        config.modbus_tcp_slave_id = self.tcp_slave_spin.value()
        return config

    def load_from_config(self, config):
        self.device_id_edit.setText(config.device_id)
        self.slave_id_spin.setValue(config.slave_id)
        self.baud_combo.setCurrentText(str(config.baud))
        self.interval_spin.setValue(config.interval_sec)
        self.parity_combo.setCurrentText(config.parity)
        self.stop_bits_combo.setCurrentText(str(config.stop_bits))
        self.wifi_ssid_edit.setText(config.wifi_ssid)
        self.wifi_pass_edit.setText(config.wifi_password)
        self.tcp_enable_chk.setChecked(config.modbus_tcp_enable == 1)
        self.tcp_ip_edit.setText(config.modbus_tcp_ip)
        self.tcp_port_spin.setValue(config.modbus_tcp_port)
        self.tcp_slave_spin.setValue(config.modbus_tcp_slave_id)

    def _run_field_action(self, action):
        """action: 'read' | 'send' | 'erase' — always the SAME one
        combined file (Wi-Fi + Device Settings + Modbus Map Sizing +
        Registers together) that Push Data to Device also uses."""
        mode = self.mw.dataPage._current_conn_mode()
        params = self.mw.dataPage._current_conn_params()
        config = self.apply_to_config(DeviceConfig())
        self.mw.mqttPage.apply_to_config(config)
        # Pull in whatever registers are currently in the Data
        # Transmission table too, so this is the exact same combined
        # file as "Push Data to Device" — not a settings-only subset.
        config.points = self.mw.dataPage.table_to_points()

        status_label = self.cfg_status_label
        buttons = [self.cfg_read_btn, self.cfg_send_btn, self.cfg_erase_btn]

        verb = {"read": "Reading…", "send": "Sending…", "erase": "Erasing…"}[action]
        status_label.setText("Settings: " + verb)
        for b in buttons:
            b.setEnabled(False)

        self.mw.log(f"Settings (Wi-Fi + Device + Modbus) {action} via {mode.upper()}…", "info")
        self.mw.statusPage.set_delivery_method(mode.upper())
        self.mw.statusPage.set_config_summary(config)

        cert_pairs = self.mw.mqttPage.get_local_cert_paths() if action == "send" else []
        self._field_worker = ConfigFieldWorker(mode, action, config, params, cert_pairs=cert_pairs)
        self._field_worker.finished.connect(
            lambda ok, msg: self._on_field_action_finished(ok, msg, status_label, buttons)
        )
        self._field_worker.start()

    def _on_field_action_finished(self, ok, message, status_label, buttons):
        for b in buttons:
            b.setEnabled(True)
        status_label.setText(f"Settings: {'OK' if ok else 'Failed'}")
        self.mw.log(message, "success" if ok else "error")
        self.mw.set_status("ok" if ok else "fail", message)
        (QMessageBox.information if ok else QMessageBox.warning)(
            self, "Device Settings", message
        )


class MQTTSettingsPage(QWidget):
    """Page 2 — Board MQTT (AWS IoT Core) settings."""

    # Maps cert key -> (line_edit attr, remote board path)
    CERT_FIELDS = {
        "ca":   ("bmq_ca_edit",   REMOTE_CA_CERT_PATH),
        "cert": ("bmq_cert_edit", REMOTE_DEVICE_CERT_PATH),
        "key":  ("bmq_key_edit",  REMOTE_PRIVATE_KEY_PATH),
    }

    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        self._local_cert_paths = {"ca": None, "cert": None, "key": None}
        layout = QVBoxLayout(self)

        step = QLabel("STEP 2 OF 4 — MQTT SETTINGS")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        board_mqtt_box = QGroupBox("Board MQTT (AWS IoT Core — board's own broker connection)")
        bmq_form = QFormLayout(board_mqtt_box)
        self.bmq_broker_edit = QLineEdit()
        self.bmq_broker_edit.setPlaceholderText("e.g. xxxxxx-ats.iot.eu-west-1.amazonaws.com")
        self.bmq_broker_edit.setToolTip("AWS IoT Core endpoint the board connects to for telemetry publish.")
        self.bmq_port_spin = QSpinBox(); self.bmq_port_spin.setRange(1, 65535); self.bmq_port_spin.setValue(8883)
        self.bmq_port_spin.setToolTip("MQTT broker port (8883 for mTLS).")
        self.bmq_client_id_edit = QLineEdit()
        self.bmq_client_id_edit.setPlaceholderText("e.g. AMSET-001")
        self.bmq_client_id_edit.setToolTip("MQTT client ID / AWS IoT Thing name.")
        self.bmq_topic_edit = QLineEdit()
        self.bmq_topic_edit.setPlaceholderText("e.g. amset/AMSET-001/data")
        self.bmq_topic_edit.setToolTip("MQTT topic the board publishes telemetry data to.")
        self.bmq_ca_edit = QLineEdit("/home/root/edb_c/linking/ca.crt")
        self.bmq_ca_edit.setToolTip("Path to CA certificate — can be a local file to upload, or a path on the board.")
        self.bmq_cert_edit = QLineEdit("/home/root/edb_c/linking/client.crt")
        self.bmq_cert_edit.setToolTip("Path to device certificate — can be a local file to upload, or a path on the board.")
        self.bmq_key_edit = QLineEdit("/home/root/edb_c/linking/private.key")
        self.bmq_key_edit.setToolTip("Path to private key — can be a local file to upload, or a path on the board.")

        ca_browse = QPushButton("Browse…")
        ca_browse.clicked.connect(lambda: self._browse_cert("ca", "CA Certificate (*.pem *.crt *.cer);;All Files (*)"))
        cert_browse = QPushButton("Browse…")
        cert_browse.clicked.connect(lambda: self._browse_cert("cert", "Device Certificate (*.pem *.crt *.cer);;All Files (*)"))
        key_browse = QPushButton("Browse…")
        key_browse.clicked.connect(lambda: self._browse_cert("key", "Private Key (*.pem *.key);;All Files (*)"))

        ca_row = QHBoxLayout(); ca_row.addWidget(self.bmq_ca_edit); ca_row.addWidget(ca_browse)
        cert_row = QHBoxLayout(); cert_row.addWidget(self.bmq_cert_edit); cert_row.addWidget(cert_browse)
        key_row = QHBoxLayout(); key_row.addWidget(self.bmq_key_edit); key_row.addWidget(key_browse)

        bmq_form.addRow("Broker:", self.bmq_broker_edit)
        bmq_form.addRow("Port:", self.bmq_port_spin)
        bmq_form.addRow("Client ID:", self.bmq_client_id_edit)
        bmq_form.addRow("Topic:", self.bmq_topic_edit)
        bmq_form.addRow("CA Cert:", ca_row)
        bmq_form.addRow("Device Cert:", cert_row)
        bmq_form.addRow("Private Key:", key_row)
        cert_note = QLabel("Use Browse to select local certificate files — they will be uploaded "
                           "to the board automatically when you push config (SSH/Serial only).")
        cert_note.setObjectName("hintLabel"); cert_note.setWordWrap(True)
        bmq_form.addRow(cert_note)
        layout.addWidget(board_mqtt_box)

        layout.addStretch()

        nav = QHBoxLayout()
        back_btn = QPushButton("← Back: Device Config")
        back_btn.setObjectName("navButton")
        back_btn.clicked.connect(lambda: self.mw.goto_page(0))
        nav.addWidget(back_btn)
        nav.addStretch()
        next_btn = QPushButton("Next: Data Transmission →")
        next_btn.setObjectName("navButton")
        next_btn.clicked.connect(lambda: self.mw.goto_page(2))
        nav.addWidget(next_btn)
        layout.addLayout(nav)

    def _browse_cert(self, cert_key, file_filter):
        """Browse for a local cert file. Stores the local path for later
        upload, and shows the filename in the UI text field."""
        filepath, _ = QFileDialog.getOpenFileName(self, "Select File", "", file_filter)
        if not filepath:
            return
        edit_attr, remote_path = self.CERT_FIELDS[cert_key]
        line_edit = getattr(self, edit_attr)
        self._local_cert_paths[cert_key] = filepath
        line_edit.setText(os.path.basename(filepath))
        line_edit.setToolTip(f"Local file: {filepath}\nWill be uploaded to: {remote_path}")

    def get_local_cert_paths(self):
        """Returns list of (local_path, remote_path) for certs that need
        uploading — i.e. where the user browsed a local file."""
        pairs = []
        for key, local_path in self._local_cert_paths.items():
            if local_path and os.path.isfile(local_path):
                _, remote_path = self.CERT_FIELDS[key]
                pairs.append((local_path, remote_path))
        return pairs

    def apply_to_config(self, config):
        config.mqtt_broker = self.bmq_broker_edit.text().strip()
        config.mqtt_port = self.bmq_port_spin.value()
        config.mqtt_client_id = self.bmq_client_id_edit.text().strip()
        config.mqtt_topic = self.bmq_topic_edit.text().strip()
        # Always write the fixed board paths into config, not local PC paths
        config.mqtt_ca_cert = REMOTE_CA_CERT_PATH
        config.mqtt_device_cert = REMOTE_DEVICE_CERT_PATH
        config.mqtt_private_key = REMOTE_PRIVATE_KEY_PATH
        return config

    def load_from_config(self, config):
        self.bmq_broker_edit.setText(config.mqtt_broker)
        self.bmq_port_spin.setValue(config.mqtt_port)
        self.bmq_client_id_edit.setText(config.mqtt_client_id)
        self.bmq_topic_edit.setText(config.mqtt_topic)
        self.bmq_ca_edit.setText(config.mqtt_ca_cert)
        self.bmq_cert_edit.setText(config.mqtt_device_cert)
        self.bmq_key_edit.setText(config.mqtt_private_key)


class DataTransmissionPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 3 OF 4 — DATA TRANSMISSION")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        reg_box = QGroupBox("Registers")
        reg_layout = QVBoxLayout(reg_box)

        src_row = QHBoxLayout()
        import_excel_btn = QPushButton("Import from Excel…")
        import_excel_btn.setToolTip("Upload a register list (.xlsx) — Label + Address columns required")
        import_excel_btn.clicked.connect(self._import_excel)
        add_row_btn = QPushButton("Add Row Manually")
        add_row_btn.clicked.connect(self._add_row_interactive)
        del_row_btn = QPushButton("Delete Selected")
        del_row_btn.clicked.connect(self._delete_selected_rows)
        src_row.addWidget(import_excel_btn)
        src_row.addWidget(add_row_btn)
        src_row.addWidget(del_row_btn)
        src_row.addStretch()
        reg_layout.addLayout(src_row)

        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.setAlternatingRowColors(True)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.DoubleClicked | QAbstractItemView.EditKeyPressed)
        self.table.itemChanged.connect(lambda _: self._update_point_count())
        reg_layout.addWidget(self.table)

        footer = QHBoxLayout()
        self.point_count_label = QLabel("0 points configured")
        self.point_count_label.setObjectName("pointCountLabel")
        footer.addWidget(self.point_count_label)
        footer.addStretch()
        hint = QLabel("Double-click a cell to edit manually — works the same whether rows came from Excel or were typed in")
        hint.setObjectName("hintLabel")
        footer.addWidget(hint)
        reg_layout.addLayout(footer)
        layout.addWidget(reg_box)

        proj_row = QHBoxLayout()
        validate_btn = QPushButton("Validate"); validate_btn.clicked.connect(self._validate)
        save_proj_btn = QPushButton("Save Project…"); save_proj_btn.clicked.connect(self._save_project)
        load_proj_btn = QPushButton("Load Project…"); load_proj_btn.clicked.connect(self._load_project)
        export_csv_btn = QPushButton("Export Full Config (.csv + .json)…"); export_csv_btn.clicked.connect(self._export_csv)
        proj_row.addWidget(validate_btn)
        proj_row.addWidget(save_proj_btn)
        proj_row.addWidget(load_proj_btn)
        proj_row.addWidget(export_csv_btn)
        proj_row.addStretch()
        layout.addLayout(proj_row)

        conn_box = QGroupBox("Delivery Method — choose whichever the site supports")
        conn_layout = QVBoxLayout(conn_box)
        self.conn_tabs = QTabWidget()
        self.conn_tabs.addTab(self._build_mqtt_tab(), "MQTT (Field / Dongle)")
        self.conn_tabs.addTab(self._build_ssh_tab(), "SSH (LAN)")
        self.conn_tabs.addTab(self._build_serial_tab(), "Serial / USB (No Network)")
        conn_layout.addWidget(self.conn_tabs)
        layout.addWidget(conn_box)

        action_row = QHBoxLayout()
        test_btn = QPushButton("Test Connection"); test_btn.clicked.connect(self._test_connection)
        refresh_btn = QPushButton("Refresh Ports"); refresh_btn.clicked.connect(self._refresh_ports)
        retry_btn = QPushButton("Retry Pending Queue"); retry_btn.clicked.connect(self._retry_queue)
        push_btn = QPushButton("Push Data to Device")
        push_btn.setObjectName("primaryButton")
        push_btn.clicked.connect(self._push_config)
        action_row.addWidget(test_btn)
        action_row.addWidget(refresh_btn)
        action_row.addStretch()
        action_row.addWidget(retry_btn)
        action_row.addWidget(push_btn)
        layout.addLayout(action_row)

        nav = QHBoxLayout()
        back_btn = QPushButton("← Back: MQTT Settings")
        back_btn.setObjectName("navButton")
        back_btn.clicked.connect(lambda: self.mw.goto_page(1))
        nav.addWidget(back_btn)
        nav.addStretch()
        status_btn = QPushButton("View Status →")
        status_btn.setObjectName("navButton")
        status_btn.clicked.connect(self._goto_status)
        nav.addWidget(status_btn)
        layout.addLayout(nav)

        self._refresh_ports()

    def _goto_status(self):
        self.mw.statusPage.set_config_summary(self._current_config())
        self.mw.goto_page(3)

    def _build_mqtt_tab(self):
        tab = QWidget()
        form = QFormLayout(tab)
        self.mqtt_broker_edit = QLineEdit("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.s1.eu.hivemq.cloud")
        self.mqtt_port_spin = QSpinBox(); self.mqtt_port_spin.setRange(1, 65535); self.mqtt_port_spin.setValue(8883)
        self.mqtt_user_edit = QLineEdit()
        self.mqtt_pass_edit = QLineEdit(); self.mqtt_pass_edit.setEchoMode(QLineEdit.Password)
        self.mqtt_remember_chk = QCheckBox("Remember password (OS keyring)")
        form.addRow("Broker:", self.mqtt_broker_edit)
        form.addRow("Port:", self.mqtt_port_spin)
        form.addRow("Username:", self.mqtt_user_edit)
        form.addRow("Password:", self.mqtt_pass_edit)
        form.addRow("", self.mqtt_remember_chk)
        note = QLabel("Recommended for field/production — works through USB/cellular dongles and NAT.")
        note.setObjectName("hintLabel"); note.setWordWrap(True)
        form.addRow(note)
        return tab

    def _build_ssh_tab(self):
        tab = QWidget()
        form = QFormLayout(tab)
        self.ssh_host_edit = QLineEdit("192.168.1.14")
        self.ssh_user_edit = QLineEdit("root")
        self.ssh_pass_edit = QLineEdit(); self.ssh_pass_edit.setEchoMode(QLineEdit.Password)
        self.ssh_remember_chk = QCheckBox("Remember password (OS keyring)")
        form.addRow("Board IP:", self.ssh_host_edit)
        form.addRow("Username:", self.ssh_user_edit)
        form.addRow("Password:", self.ssh_pass_edit)
        form.addRow("", self.ssh_remember_chk)
        note = QLabel("Only works when the board has a directly reachable IP (LAN/bench).")
        note.setObjectName("hintLabel"); note.setWordWrap(True)
        form.addRow(note)
        return tab

    def _build_serial_tab(self):
        tab = QWidget()
        form = QFormLayout(tab)
        self.serial_port_combo = QComboBox()
        self.serial_baud_combo = QComboBox()
        self.serial_baud_combo.addItems(["9600", "38400", "57600", "115200"])
        self.serial_baud_combo.setCurrentText("115200")
        self.serial_user_edit = QLineEdit("root")
        self.serial_pass_edit = QLineEdit(); self.serial_pass_edit.setEchoMode(QLineEdit.Password)
        form.addRow("Serial Port:", self.serial_port_combo)
        form.addRow("Console Baud:", self.serial_baud_combo)
        form.addRow("Login Username:", self.serial_user_edit)
        form.addRow("Login Password:", self.serial_pass_edit)
        note = QLabel("Direct USB cable to the board's console port — no network needed at all. "
                       "Use 'Refresh Ports' if your cable isn't listed. Baud here is the CONSOLE "
                       "baud (often 115200), not the RS485/Modbus field baud from Page 1.")
        note.setObjectName("hintLabel"); note.setWordWrap(True)
        form.addRow(note)
        return tab

    def _refresh_ports(self):
        self.serial_port_combo.clear()
        try:
            ports = SerialTransport.list_ports()
        except Exception:
            ports = []
        if not ports:
            self.serial_port_combo.addItem("No ports found")
        for device, desc in ports:
            # On the target Linux board, USB-CDC serial ports enumerate as
            # /dev/ttyACM0, /dev/ttyACM1, /dev/ttyACM2 etc. Show that name
            # plainly in the dropdown so it matches what's on the device.
            label = device
            if "ACM" in device.upper() or "USB" in device.upper():
                label = device
            self.serial_port_combo.addItem(f"{label} — {desc}", userData=device)
        self.mw.log(f"Refreshed serial ports — {len(ports)} found", "info")

    def _add_row(self, slave_id=0, label="", address=0,
                 register_type="holding", data_type="uint16"):
        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(str(slave_id)))
        self.table.setItem(row, 1, QTableWidgetItem(str(label)))
        self.table.setItem(row, 2, QTableWidgetItem(str(address)))
        # Type dropdown
        type_combo = QComboBox()
        type_combo.addItems(REG_TYPES)
        type_combo.setCurrentText(register_type)
        self.table.setCellWidget(row, 3, type_combo)
        # Data Type dropdown
        dtype_combo = QComboBox()
        dtype_combo.addItems(DATA_TYPES)
        dtype_combo.setCurrentText(data_type)
        self.table.setCellWidget(row, 4, dtype_combo)
        self._update_point_count()

    def _add_row_interactive(self):
        self._add_row()
        row = self.table.rowCount() - 1
        self.table.setCurrentCell(row, 1)
        self.table.editItem(self.table.item(row, 1))

    def _delete_selected_rows(self):
        rows = sorted({idx.row() for idx in self.table.selectedIndexes()}, reverse=True)
        if not rows:
            self.mw.log("No rows selected", "warn")
            return
        for r in rows:
            self.table.removeRow(r)
        self._update_point_count()
        self.mw.log(f"Deleted {len(rows)} row(s)", "info")

    def _update_point_count(self):
        n = self.table.rowCount()
        self.point_count_label.setText(f"{n} point{'s' if n != 1 else ''} configured")

    def _import_excel(self):
        filepath, _ = QFileDialog.getOpenFileName(self, "Import Registers from Excel", "", "Excel Files (*.xlsx *.xls)")
        if not filepath:
            return
        try:
            points, warnings = import_excel(filepath)
        except Exception as e:
            QMessageBox.critical(self, "Import Failed", str(e)); return
        if not points:
            QMessageBox.warning(self, "Import Failed", "No valid rows found — check Label/Address columns."); return

        replace = True
        if self.table.rowCount() > 0:
            choice = QMessageBox.question(
                self, "Import from Excel",
                f"Found {len(points)} points.\nTable already has {self.table.rowCount()} row(s).\n"
                f"Replace, or append below?",
                buttons=QMessageBox.Yes | QMessageBox.No | QMessageBox.Cancel,
            )
            if choice == QMessageBox.Cancel:
                return
            replace = (choice == QMessageBox.Yes)
        if replace:
            self.table.setRowCount(0)
        for p in points:
            self._add_row(p.slave_id, p.label, p.address,
                          p.register_type, p.data_type)
        self._update_point_count()
        self.mw.log(f"Imported {len(points)} points from Excel: {filepath}", "success")
        if warnings:
            self.mw.log("Import warnings:\n" + "\n".join(warnings), "warn")
            QMessageBox.information(self, "Imported with warnings",
                                     f"{len(points)} imported, {len(warnings)} row(s) skipped/adjusted — see Status page.")

    def table_to_points(self):
        points = []
        for row in range(self.table.rowCount()):
            sid_text = self.table.item(row, 0).text() if self.table.item(row, 0) else "0"
            label = self.table.item(row, 1).text() if self.table.item(row, 1) else ""
            addr_text = self.table.item(row, 2).text() if self.table.item(row, 2) else "0"
            type_w = self.table.cellWidget(row, 3)
            reg_type = type_w.currentText() if type_w else "holding"
            dtype_w = self.table.cellWidget(row, 4)
            data_type = dtype_w.currentText() if dtype_w else "uint16"
            try:
                slave_id = int(sid_text)
            except ValueError:
                slave_id = 0
            try:
                address = int(addr_text)
            except ValueError:
                address = -1
            points.append(RegisterPoint(
                label=label, address=address, slave_id=slave_id,
                register_type=reg_type, data_type=data_type,
            ))
        return points

    def load_points(self, points):
        self.table.setRowCount(0)
        for p in points:
            self._add_row(p.slave_id, p.label, p.address,
                          p.register_type, p.data_type)
        self._update_point_count()

    def _current_config(self):
        config = self.mw.devicePage.apply_to_config(DeviceConfig())
        self.mw.mqttPage.apply_to_config(config)
        config.points = self.table_to_points()
        return config

    def _validate(self):
        config = self._current_config()
        errors = config.validate_all()
        if errors:
            self.mw.log("Validation failed:\n" + "\n".join(errors), "error")
            QMessageBox.warning(self, "Validation Failed", "\n".join(errors[:15]) + ("\n…" if len(errors) > 15 else ""))
            return False
        self.mw.log(f"Validation OK — {len(config.points)} points, no errors.", "success")
        return True

    def _save_project(self):
        if not self._validate():
            return
        config = self._current_config()
        filepath, _ = QFileDialog.getSaveFileName(self, "Save Project", f"{config.device_id}.json", "JSON Files (*.json)")
        if filepath:
            config.save_json(filepath)
            self.mw.log(f"Project saved: {filepath}", "success")

    def _load_project(self):
        filepath, _ = QFileDialog.getOpenFileName(self, "Load Project", "", "JSON Files (*.json)")
        if not filepath:
            return
        try:
            config = DeviceConfig.load_json(filepath)
            self.mw.devicePage.load_from_config(config)
            self.mw.mqttPage.load_from_config(config)
            self.load_points(config.points)
            self.mw.log(f"Loaded project: {filepath}", "success")
        except Exception as e:
            QMessageBox.critical(self, "Load Failed", str(e))

    def _export_csv(self):
        if not self._validate():
            return
        config = self._current_config()
        base_path, _ = QFileDialog.getSaveFileName(
            self, "Export Full Config (creates both .csv and .json)",
            f"{config.device_id}_config", "All Files (*)"
        )
        if not base_path:
            return
        # Strip whatever extension the dialog added — we always write both.
        if base_path.lower().endswith(".csv") or base_path.lower().endswith(".json"):
            base_path = base_path.rsplit(".", 1)[0]

        csv_path = base_path + ".csv"
        json_path = base_path + ".json"
        config.save_full_config_csv(csv_path)
        config.save_full_config_json(json_path)

        self.mw.log(
            f"Full config exported as both CSV and JSON:\n"
            f"  {csv_path}\n  {json_path}\n"
            f"Includes Wi-Fi, Device Settings, and {len(config.points)} "
            f"register(s) with per-register slave_id/type/data_type.",
            "success"
        )
        QMessageBox.information(
            self, "Export Complete",
            f"Saved:\n{csv_path}\n{json_path}\n\n"
            f"{len(config.points)} registers exported."
        )

    def _current_conn_mode(self):
        idx = self.conn_tabs.currentIndex()
        return ["mqtt", "ssh", "serial"][idx]

    def _current_conn_params(self):
        mode = self._current_conn_mode()
        if mode == "mqtt":
            return {"broker": self.mqtt_broker_edit.text().strip(), "port": self.mqtt_port_spin.value(),
                    "username": self.mqtt_user_edit.text().strip(), "password": self.mqtt_pass_edit.text(),
                    "ack_timeout": 15}
        if mode == "ssh":
            return {"host": self.ssh_host_edit.text().strip(), "username": self.ssh_user_edit.text().strip(),
                    "password": self.ssh_pass_edit.text()}
        port_data = self.serial_port_combo.currentData()
        return {"port": port_data or self.serial_port_combo.currentText(),
                "console_baud": int(self.serial_baud_combo.currentText()),
                "username": self.serial_user_edit.text().strip() or "root",
                "password": self.serial_pass_edit.text()}

    def _persist_credentials(self):
        if self.mqtt_remember_chk.isChecked() and self.mqtt_pass_edit.text():
            credentials.save_mqtt_password(self.mqtt_pass_edit.text())
        if self.ssh_remember_chk.isChecked() and self.ssh_pass_edit.text():
            credentials.save_ssh_password(self.ssh_pass_edit.text())

    def load_saved_credentials(self):
        mqtt_pw = credentials.get_mqtt_password()
        if mqtt_pw:
            self.mqtt_pass_edit.setText(mqtt_pw); self.mqtt_remember_chk.setChecked(True)
        ssh_pw = credentials.get_ssh_password()
        if ssh_pw:
            self.ssh_pass_edit.setText(ssh_pw); self.ssh_remember_chk.setChecked(True)

    def _test_connection(self):
        mode = self._current_conn_mode()
        params = self._current_conn_params()
        self.mw.log(f"Testing {mode.upper()} connection…", "info")
        self.mw.statusPage.set_delivery_method(mode.upper())
        self.mw.set_status("testing", f"Testing {mode.upper()} connection…")
        self._test_worker = TestWorker(mode, params)
        self._test_worker.finished.connect(self._on_test_finished)
        self._test_worker.start()

    def _on_test_finished(self, ok, message):
        self.mw.log(message, "success" if ok else "error")
        self.mw.set_status("ok" if ok else "fail", message)
        (QMessageBox.information if ok else QMessageBox.warning)(self, "Connection Test", message)

    def _push_config(self):
        if not self._validate():
            return
        self._persist_credentials()
        config = self._current_config()
        mode = self._current_conn_mode()
        params = self._current_conn_params()
        cert_pairs = self.mw.mqttPage.get_local_cert_paths()

        self.mw.goto_page(3)
        self.mw.statusPage.set_delivery_method(mode.upper())
        self.mw.statusPage.set_config_summary(config)
        self.mw.set_status("busy", f"Pushing config via {mode.upper()}…")
        self.progress = QProgressDialog(f"Pushing config via {mode.upper()}…", None, 0, 0, self)
        self.progress.setWindowModality(Qt.WindowModal)
        self.progress.show()

        self.worker = PushWorker(mode, config, params, cert_pairs=cert_pairs)
        self.worker.finished_ok.connect(self._on_push_ok)
        self.worker.finished_fail.connect(self._on_push_fail)
        self.worker.start()

    def _on_push_ok(self, message):
        self.progress.close()
        self.mw.log("SUCCESS: " + message, "success")
        self.mw.set_status("ok", message)

    def _on_push_fail(self, message):
        self.progress.close()
        self.mw.log("FAILED: " + message, "error")
        self.mw.set_status("fail", message)

    def _retry_queue(self):
        pending = offline_queue.list_queued()
        if not pending:
            QMessageBox.information(self, "Retry Queue", "No pending configs."); return
        if self._current_conn_mode() != "mqtt":
            QMessageBox.information(self, "Retry Queue", "Queued configs are for MQTT — switch to the MQTT tab."); return
        params = self._current_conn_params()
        self.mw.log("Retrying pending queue…", "info")
        self._retry_worker = RetryQueueWorker(params)
        self._retry_worker.finished.connect(self._on_retry_finished)
        self._retry_worker.start()

    def _on_retry_finished(self, results):
        self.mw.log("Retry queue results:\n" + "\n".join(results), "info")
        QMessageBox.information(self, "Retry Queue", "\n".join(results))


class StatusPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 4 OF 4 — STATUS")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        self.delivery_method_label = QLabel("Delivery Method: —")
        self.delivery_method_label.setStyleSheet("font-weight: 600; color: #2c3e50; font-size: 11pt;")
        layout.addWidget(self.delivery_method_label)

        summary_box = QGroupBox("Device Config Used (this session)")
        summary_box.setObjectName("summaryCard")
        summary_layout = QVBoxLayout(summary_box)
        self.summary_label = QLabel(
            "No config sent yet — push data, or send Wi-Fi/Config, to see a recap here."
        )
        self.summary_label.setObjectName("summaryText")
        self.summary_label.setWordWrap(True)
        summary_layout.addWidget(self.summary_label)
        layout.addWidget(summary_box)

        self.status_pill = QFrame()
        self.status_pill.setObjectName("statusPill")
        pill_layout = QVBoxLayout(self.status_pill)
        self.status_headline = QLabel("Idle")
        self.status_headline.setStyleSheet("font-size: 16pt; font-weight: 700; color: #12261b;")
        self.status_detail = QLabel("No action taken yet.")
        self.status_detail.setStyleSheet("color: #12261b;")
        self.status_detail.setWordWrap(True)
        pill_layout.addWidget(self.status_headline)
        pill_layout.addWidget(self.status_detail)
        self._paint_status("idle")
        layout.addWidget(self.status_pill)

        log_box = QGroupBox("Full Log")
        log_layout = QVBoxLayout(log_box)
        self.log_view = QTextEdit()
        self.log_view.setObjectName("logView")
        self.log_view.setReadOnly(True)
        log_layout.addWidget(self.log_view)
        layout.addWidget(log_box, stretch=1)

        nav = QHBoxLayout()
        back_btn = QPushButton("← Back: Data Transmission")
        back_btn.setObjectName("navButton")
        back_btn.clicked.connect(lambda: self.mw.goto_page(2))
        nav.addWidget(back_btn)

        erase_btn = QPushButton("Erase All Data")
        erase_btn.setStyleSheet("background-color: #e0625a; color: white; font-weight: 600; border: none; padding: 9px 20px;")
        erase_btn.clicked.connect(self._erase_all_data)
        nav.addWidget(erase_btn)

        nav.addStretch()

        self.close_btn = QPushButton("Close Tool")
        self.close_btn.setObjectName("primaryButton")
        self.close_btn.clicked.connect(self.mw.close)
        self.close_btn.setVisible(False)
        nav.addWidget(self.close_btn)

        layout.addLayout(nav)

    def set_delivery_method(self, mode_label):
        self.delivery_method_label.setText(f"Delivery Method: {mode_label}")

    def set_config_summary(self, config):
        wifi_line = (f"Wi-Fi SSID: <b>{config.wifi_ssid}</b>" if config.wifi_ssid
                     else "Wi-Fi: not set (USB dongle / Ethernet)")
        device_line = (f"Device: <b>{config.device_id}</b> &nbsp;|&nbsp; "
                       f"Default Slave ID: {config.slave_id} "
                       f"&nbsp;|&nbsp; Baud: {config.baud} &nbsp;|&nbsp; "
                       f"Parity: {config.parity} "
                       f"&nbsp;|&nbsp; Stop Bits: {config.stop_bits} "
                       f"&nbsp;|&nbsp; Poll: {config.interval_sec}s")
        reg_line = f"Registers configured: <b>{len(config.points)}</b>"
        self.summary_label.setTextFormat(Qt.RichText)
        self.summary_label.setText(
            wifi_line + "<br>" + device_line + "<br>" + reg_line
        )

    def _paint_status(self, state):
        colors = {"idle": "#e2e5e9", "testing": "#cfe4fb", "busy": "#fbe8bd",
                  "ok": "#c9f2d8", "fail": "#fbd4d1"}
        self.status_pill.setStyleSheet(f"QFrame#statusPill {{ background-color: {colors.get(state, '#e2e5e9')}; }}")

    def set_status(self, state, message):
        headlines = {"idle": "Idle", "testing": "Testing…", "busy": "Working…",
                     "ok": "Success", "fail": "Attention Needed"}
        self._paint_status(state)
        self.status_headline.setText(headlines.get(state, state))
        self.status_detail.setText(message)
        self.close_btn.setVisible(state == "ok")

    def _erase_all_data(self):
        confirm = QMessageBox.question(
            self, "Erase All Data",
            "This will permanently delete all saved credentials and the "
            "locally queued (pending retry) configs stored by this tool.\n\n"
            "This does NOT affect data already on the device — only files "
            "kept on this PC. Continue?",
            buttons=QMessageBox.Yes | QMessageBox.No,
        )
        if confirm != QMessageBox.Yes:
            return
        removed = []
        try:
            for item in offline_queue.list_queued():
                offline_queue.remove(item.filepath)
            removed.append("pending retry queue")
        except Exception as e:
            self.log(f"Could not clear retry queue: {e}", "error")
        try:
            credentials.clear_all()
            removed.append("saved credentials")
        except Exception as e:
            self.log(f"Could not clear saved credentials: {e}", "error")
        self.log("Erased: " + ", ".join(removed) if removed else "Nothing to erase.", "warn")
        QMessageBox.information(self, "Erase All Data", "Local tool data erased.")

    def log(self, message, level="info"):
        import html
        from datetime import datetime
        colors = {"info": "#9fb3c8", "success": "#57c97a", "warn": "#e0b342", "error": "#e0625a"}
        color = colors.get(level, colors["info"])
        ts = datetime.now().strftime("%H:%M:%S")
        safe = html.escape(message).replace("\n", "<br>")
        self.log_view.append(f'<span style="color:#6b7684;">[{ts}]</span> <span style="color:{color};">{safe}</span>')


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Smart RTU Tool — STM32MP157F-DK2 Edition")
        self.resize(1080, 800)
        self.setStyleSheet(APP_STYLESHEET)

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        nav_bar = QFrame()
        nav_bar.setObjectName("topNavBar")
        nav_layout = QHBoxLayout(nav_bar)
        nav_layout.setContentsMargins(14, 8, 14, 8)
        nav_layout.setSpacing(8)

        self.nav_buttons = []
        nav_titles = ["1. Device Config", "2. MQTT Settings", "3. Data Transmission", "4. Status"]
        for i, title in enumerate(nav_titles):
            btn = QPushButton(title)
            btn.setObjectName("topNavButton")
            btn.setCheckable(True)
            btn.clicked.connect(lambda _, idx=i: self.goto_page(idx))
            nav_layout.addWidget(btn)
            self.nav_buttons.append(btn)
        nav_layout.addStretch()
        outer.addWidget(nav_bar)

        self.stack = QStackedWidget()
        # statusPage is created first: DataTransmissionPage.__init__() calls
        # _refresh_ports(), which logs through self.mw.log() -> self.statusPage.log().
        # If statusPage didn't exist yet, that raised AttributeError.
        self.statusPage = StatusPage(self)
        self.devicePage = DeviceConfigPage(self)
        self.mqttPage = MQTTSettingsPage(self)
        self.dataPage = DataTransmissionPage(self)
        self.stack.addWidget(self.devicePage)   # index 0
        self.stack.addWidget(self.mqttPage)     # index 1
        self.stack.addWidget(self.dataPage)     # index 2
        self.stack.addWidget(self.statusPage)   # index 3
        outer.addWidget(self.stack)

        self.dataPage.load_saved_credentials()
        self.statusBar().showMessage("Ready", 3000)
        self.goto_page(0)

    def goto_page(self, index):
        self.stack.setCurrentIndex(index)
        for i, btn in enumerate(self.nav_buttons):
            btn.setChecked(i == index)

    def log(self, message, level="info"):
        self.statusPage.log(message, level)

    def set_status(self, state, message):
        self.statusPage.set_status(state, message)


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
