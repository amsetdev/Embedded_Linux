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
import os

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QTableWidget, QTableWidgetItem, QPushButton, QLabel, QLineEdit,
    QComboBox, QSpinBox, QTabWidget, QGroupBox, QFormLayout, QMessageBox,
    QFileDialog, QTextEdit, QCheckBox, QHeaderView, QProgressDialog,
    QAbstractItemView, QStackedWidget, QFrame
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from core.register_model import DeviceConfig, RegisterPoint, VALID_REG_TYPES, VALID_DATA_TYPES
from core.ssh_transport import SSHTransport
from core.mqtt_transport import MQTTTransport
from core.serial_transport import SerialTransport
from core.excel_import import import_excel
from core import offline_queue, credentials


COLUMNS = ["Label", "Address", "Register Type", "Data Type", "Unit", "Scale"]

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
"""


class PushWorker(QThread):
    finished_ok = pyqtSignal(str)
    finished_fail = pyqtSignal(str)

    def __init__(self, mode, config, conn_params):
        super().__init__()
        self.mode = mode
        self.config = config
        self.conn_params = conn_params

    def run(self):
        csv_content = self.config.to_csv_string()
        try:
            if self.mode == "mqtt":
                transport = MQTTTransport(
                    broker=self.conn_params["broker"], port=self.conn_params["port"],
                    username=self.conn_params["username"], password=self.conn_params["password"],
                )
                result = transport.push_config(self.config.device_id, csv_content,
                                                 wait_ack_seconds=self.conn_params.get("ack_timeout", 15))
                if result.ok:
                    self.finished_ok.emit(result.message)
                else:
                    offline_queue.enqueue(self.config.device_id, csv_content)
                    self.finished_fail.emit(result.message + "\n\nConfig queued locally for retry.")

            elif self.mode == "ssh":
                transport = SSHTransport(host=self.conn_params["host"],
                                          username=self.conn_params["username"],
                                          password=self.conn_params["password"])
                push_result = transport.push_csv(csv_content)
                if not push_result.ok:
                    self.finished_fail.emit(push_result.message)
                    return
                restart_result = transport.restart_app()
                if restart_result.ok:
                    self.finished_ok.emit(push_result.message + "\n" + restart_result.message)
                else:
                    self.finished_fail.emit(push_result.message + "\nRestart failed: " + restart_result.message)

            elif self.mode == "serial":
                transport = SerialTransport(port=self.conn_params["port"],
                                             baud=self.conn_params.get("console_baud", 115200))
                result = transport.push_csv(
                    csv_content,
                    username=self.conn_params.get("username", "root"),
                    password=self.conn_params.get("password"),
                )
                if result.ok:
                    self.finished_ok.emit(result.message)
                else:
                    self.finished_fail.emit(result.message)
        except Exception as e:
            self.finished_fail.emit(f"Unexpected error: {e}")


class TestWorker(QThread):
    finished = pyqtSignal(bool, str)

    def __init__(self, mode, conn_params):
        super().__init__()
        self.mode = mode
        self.conn_params = conn_params

    def run(self):
        try:
            if self.mode == "mqtt":
                r = MQTTTransport(self.conn_params["broker"], self.conn_params["port"],
                                   self.conn_params["username"], self.conn_params["password"]).test_connection()
            elif self.mode == "ssh":
                r = SSHTransport(self.conn_params["host"], self.conn_params["username"],
                                  self.conn_params["password"]).test_connection()
            else:
                r = SerialTransport(self.conn_params["port"],
                                     self.conn_params.get("console_baud", 115200)).test_connection()
            self.finished.emit(r.ok, r.message)
        except Exception as e:
            self.finished.emit(False, str(e))


class DeviceConfigPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 1 OF 3 — DEVICE CONFIG")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        wifi_box = QGroupBox("Wi-Fi Settings (board's own network, if applicable)")
        wifi_form = QFormLayout(wifi_box)
        self.wifi_ssid_edit = QLineEdit()
        self.wifi_pass_edit = QLineEdit(); self.wifi_pass_edit.setEchoMode(QLineEdit.Password)
        self.wifi_show_chk = QCheckBox("Show password")
        self.wifi_show_chk.toggled.connect(
            lambda on: self.wifi_pass_edit.setEchoMode(QLineEdit.Normal if on else QLineEdit.Password)
        )
        wifi_form.addRow("SSID:", self.wifi_ssid_edit)
        wifi_form.addRow("Password:", self.wifi_pass_edit)
        wifi_form.addRow("", self.wifi_show_chk)
        note = QLabel("Leave blank if this device uses a USB/cellular dongle "
                       "or wired Ethernet instead of Wi-Fi.")
        note.setObjectName("hintLabel"); note.setWordWrap(True)
        wifi_form.addRow(note)
        layout.addWidget(wifi_box)

        modbus_box = QGroupBox("Device Settings")
        form = QFormLayout(modbus_box)
        self.device_id_edit = QLineEdit("AMSET-001")
        self.slave_id_spin = QSpinBox(); self.slave_id_spin.setRange(1, 247); self.slave_id_spin.setValue(1)
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200"])
        self.baud_combo.setCurrentText("9600")
        self.interval_spin = QSpinBox(); self.interval_spin.setRange(1, 3600); self.interval_spin.setValue(30)
        self.interval_spin.setSuffix(" s")
        form.addRow("Device Name / ID:", self.device_id_edit)
        form.addRow("Modbus Slave ID:", self.slave_id_spin)
        form.addRow("Baud Rate:", self.baud_combo)
        form.addRow("Poll Interval:", self.interval_spin)
        layout.addWidget(modbus_box)

        sizing_box = QGroupBox("Modbus Map Sizing")
        sizing_outer = QVBoxLayout(sizing_box)

        top_row = QHBoxLayout()
        self.coils_spin = QSpinBox(); self.coils_spin.setRange(0, 9999)
        self.alerts_spin = QSpinBox(); self.alerts_spin.setRange(0, 9999)
        top_form = QFormLayout()
        top_form.addRow("Coils:", self.coils_spin)
        top_form.addRow("Alerts:", self.alerts_spin)
        top_row.addLayout(top_form)
        top_row.addStretch()
        sizing_outer.addLayout(top_row)

        reg_row = QHBoxLayout()

        holding_box = QGroupBox("Holding Registers")
        holding_form = QFormLayout(holding_box)
        self.holding_integers_spin = QSpinBox(); self.holding_integers_spin.setRange(0, 9999)
        self.holding_decimals_spin = QSpinBox(); self.holding_decimals_spin.setRange(0, 9999)
        self.holding_double_integers_spin = QSpinBox(); self.holding_double_integers_spin.setRange(0, 9999)
        holding_form.addRow("Integers:", self.holding_integers_spin)
        holding_form.addRow("Decimals:", self.holding_decimals_spin)
        holding_form.addRow("Double Integers:", self.holding_double_integers_spin)
        reg_row.addWidget(holding_box)

        input_box = QGroupBox("Input Registers")
        input_form = QFormLayout(input_box)
        self.input_integers_spin = QSpinBox(); self.input_integers_spin.setRange(0, 9999)
        self.input_decimals_spin = QSpinBox(); self.input_decimals_spin.setRange(0, 9999)
        self.input_double_integers_spin = QSpinBox(); self.input_double_integers_spin.setRange(0, 9999)
        input_form.addRow("Integers:", self.input_integers_spin)
        input_form.addRow("Decimals:", self.input_decimals_spin)
        input_form.addRow("Double Integers:", self.input_double_integers_spin)
        reg_row.addWidget(input_box)

        sizing_outer.addLayout(reg_row)

        bottom_row = QHBoxLayout()
        self.parity_combo = QComboBox(); self.parity_combo.addItems(["None", "Even", "Odd"])
        self.stop_bits_combo = QComboBox(); self.stop_bits_combo.addItems(["1", "2"])
        bottom_form = QFormLayout()
        bottom_form.addRow("Parity:", self.parity_combo)
        bottom_form.addRow("Stop Bits:", self.stop_bits_combo)
        bottom_row.addLayout(bottom_form)
        bottom_row.addStretch()
        sizing_outer.addLayout(bottom_row)

        layout.addWidget(sizing_box)

        layout.addStretch()

        nav = QHBoxLayout()
        nav.addStretch()
        next_btn = QPushButton("Next: Data Transmission →")
        next_btn.setObjectName("navButton")
        next_btn.clicked.connect(lambda: self.mw.goto_page(1))
        nav.addWidget(next_btn)
        layout.addLayout(nav)

    def apply_to_config(self, config):
        config.device_id = self.device_id_edit.text().strip() or "AMSET-001"
        config.slave_id = self.slave_id_spin.value()
        config.baud = int(self.baud_combo.currentText())
        config.interval_sec = self.interval_spin.value()
        config.coils = self.coils_spin.value()
        config.alerts = self.alerts_spin.value()
        config.holding_integers = self.holding_integers_spin.value()
        config.holding_decimals = self.holding_decimals_spin.value()
        config.holding_double_integers = self.holding_double_integers_spin.value()
        config.input_integers = self.input_integers_spin.value()
        config.input_decimals = self.input_decimals_spin.value()
        config.input_double_integers = self.input_double_integers_spin.value()
        config.parity = self.parity_combo.currentText()
        config.stop_bits = int(self.stop_bits_combo.currentText())
        return config


class DataTransmissionPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 2 OF 3 — DATA TRANSMISSION")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        reg_box = QGroupBox("Registers")
        reg_layout = QVBoxLayout(reg_box)

        src_row = QHBoxLayout()
        import_excel_btn = QPushButton("📥 Import from Excel…")
        import_excel_btn.setToolTip("Upload a register list (.xlsx) — Label + Address columns required")
        import_excel_btn.clicked.connect(self._import_excel)
        add_row_btn = QPushButton("➕ Add Row Manually")
        add_row_btn.clicked.connect(self._add_row_interactive)
        del_row_btn = QPushButton("🗑 Delete Selected")
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
        validate_btn = QPushButton("✔ Validate"); validate_btn.clicked.connect(self._validate)
        save_proj_btn = QPushButton("💾 Save Project…"); save_proj_btn.clicked.connect(self._save_project)
        load_proj_btn = QPushButton("📂 Load Project…"); load_proj_btn.clicked.connect(self._load_project)
        export_csv_btn = QPushButton("⬇ Export registers.csv…"); export_csv_btn.clicked.connect(self._export_csv)
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
        test_btn = QPushButton("🔌 Test Connection"); test_btn.clicked.connect(self._test_connection)
        refresh_btn = QPushButton("🔄 Refresh Ports"); refresh_btn.clicked.connect(self._refresh_ports)
        retry_btn = QPushButton("🔁 Retry Pending Queue"); retry_btn.clicked.connect(self._retry_queue)
        push_btn = QPushButton("🚀 Push Data to Device")
        push_btn.setObjectName("primaryButton")
        push_btn.clicked.connect(self._push_config)
        action_row.addWidget(test_btn)
        action_row.addWidget(refresh_btn)
        action_row.addStretch()
        action_row.addWidget(retry_btn)
        action_row.addWidget(push_btn)
        layout.addLayout(action_row)

        nav = QHBoxLayout()
        back_btn = QPushButton("← Back: Device Config")
        back_btn.setObjectName("navButton")
        back_btn.clicked.connect(lambda: self.mw.goto_page(0))
        nav.addWidget(back_btn)
        nav.addStretch()
        status_btn = QPushButton("View Status →")
        status_btn.setObjectName("navButton")
        status_btn.clicked.connect(lambda: self.mw.goto_page(2))
        nav.addWidget(status_btn)
        layout.addLayout(nav)

        self._refresh_ports()

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

    def _add_row(self, label="", address=0, reg_type="holding", data_type="word", unit="", scale=1.0):
        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(str(label)))
        self.table.setItem(row, 1, QTableWidgetItem(str(address)))
        reg_combo = QComboBox(); reg_combo.addItems(sorted(VALID_REG_TYPES)); reg_combo.setCurrentText(reg_type)
        self.table.setCellWidget(row, 2, reg_combo)
        dtype_combo = QComboBox(); dtype_combo.addItems(sorted(VALID_DATA_TYPES)); dtype_combo.setCurrentText(data_type)
        self.table.setCellWidget(row, 3, dtype_combo)
        self.table.setItem(row, 4, QTableWidgetItem(str(unit)))
        self.table.setItem(row, 5, QTableWidgetItem(str(scale)))
        self._update_point_count()

    def _add_row_interactive(self):
        self._add_row()
        row = self.table.rowCount() - 1
        self.table.setCurrentCell(row, 0)
        self.table.editItem(self.table.item(row, 0))

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
            self._add_row(p.label, p.address, p.register_type, p.data_type, p.unit, p.scale)
        self._update_point_count()
        self.mw.log(f"Imported {len(points)} points from Excel: {filepath}", "success")
        if warnings:
            self.mw.log("Import warnings:\n" + "\n".join(warnings), "warn")
            QMessageBox.information(self, "Imported with warnings",
                                     f"{len(points)} imported, {len(warnings)} row(s) skipped/adjusted — see Status page.")

    def table_to_points(self):
        points = []
        for row in range(self.table.rowCount()):
            label = self.table.item(row, 0).text() if self.table.item(row, 0) else ""
            addr_text = self.table.item(row, 1).text() if self.table.item(row, 1) else "0"
            try:
                address = int(addr_text)
            except ValueError:
                address = -1
            reg_type = self.table.cellWidget(row, 2).currentText()
            data_type = self.table.cellWidget(row, 3).currentText()
            unit = self.table.item(row, 4).text() if self.table.item(row, 4) else ""
            scale_text = self.table.item(row, 5).text() if self.table.item(row, 5) else "1.0"
            try:
                scale = float(scale_text)
            except ValueError:
                scale = 1.0
            points.append(RegisterPoint(label=label, address=address, register_type=reg_type,
                                        data_type=data_type, unit=unit, scale=scale))
        return points

    def load_points(self, points):
        self.table.setRowCount(0)
        for p in points:
            self._add_row(p.label, p.address, p.register_type, p.data_type, p.unit, p.scale)
        self._update_point_count()

    def _current_config(self):
        config = self.mw.devicePage.apply_to_config(DeviceConfig())
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
            self.mw.devicePage.device_id_edit.setText(config.device_id)
            self.mw.devicePage.slave_id_spin.setValue(config.slave_id)
            self.mw.devicePage.baud_combo.setCurrentText(str(config.baud))
            self.mw.devicePage.interval_spin.setValue(config.interval_sec)
            self.load_points(config.points)
            self.mw.log(f"Loaded project: {filepath}", "success")
        except Exception as e:
            QMessageBox.critical(self, "Load Failed", str(e))

    def _export_csv(self):
        if not self._validate():
            return
        config = self._current_config()
        filepath, _ = QFileDialog.getSaveFileName(self, "Export registers.csv", "registers.csv", "CSV Files (*.csv)")
        if filepath:
            config.save_csv(filepath)
            self.mw.log(f"registers.csv exported: {filepath}", "success")

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

        self.mw.goto_page(2)
        self.mw.statusPage.set_delivery_method(mode.upper())
        self.mw.set_status("busy", f"Pushing config via {mode.upper()}…")
        self.progress = QProgressDialog(f"Pushing config via {mode.upper()}…", None, 0, 0, self)
        self.progress.setWindowModality(Qt.WindowModal)
        self.progress.show()

        self.worker = PushWorker(mode, config, params)
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
        transport = MQTTTransport(params["broker"], params["port"], params["username"], params["password"])
        results = []
        for item in pending:
            r = transport.push_config(item.device_id, item.csv_content)
            if r.ok:
                offline_queue.remove(item.filepath)
                results.append(f"{item.device_id}: delivered")
            else:
                results.append(f"{item.device_id}: still pending ({r.message})")
        self.mw.log("Retry queue results:\n" + "\n".join(results), "info")
        QMessageBox.information(self, "Retry Queue", "\n".join(results))


class StatusPage(QWidget):
    def __init__(self, main_window):
        super().__init__()
        self.mw = main_window
        layout = QVBoxLayout(self)

        step = QLabel("STEP 3 OF 3 — STATUS")
        step.setObjectName("stepLabel")
        layout.addWidget(step)

        self.delivery_method_label = QLabel("Delivery Method: —")
        self.delivery_method_label.setStyleSheet("font-weight: 600; color: #2c3e50;")
        layout.addWidget(self.delivery_method_label)

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
        back_btn.clicked.connect(lambda: self.mw.goto_page(1))
        nav.addWidget(back_btn)

        erase_btn = QPushButton("🗑 Erase All Data")
        erase_btn.setStyleSheet("background-color: #e0625a; color: white; font-weight: 600; border: none; padding: 9px 20px;")
        erase_btn.clicked.connect(self._erase_all_data)
        nav.addWidget(erase_btn)

        nav.addStretch()

        self.close_btn = QPushButton("✔ Close Tool")
        self.close_btn.setObjectName("primaryButton")
        self.close_btn.clicked.connect(self.mw.close)
        self.close_btn.setVisible(False)
        nav.addWidget(self.close_btn)

        layout.addLayout(nav)

    def set_delivery_method(self, mode_label):
        self.delivery_method_label.setText(f"Delivery Method: {mode_label}")

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

        self.stack = QStackedWidget()
        # statusPage is created first: DataTransmissionPage.__init__() calls
        # _refresh_ports(), which logs through self.mw.log() -> self.statusPage.log().
        # If statusPage didn't exist yet, that raised AttributeError.
        self.statusPage = StatusPage(self)
        self.devicePage = DeviceConfigPage(self)
        self.dataPage = DataTransmissionPage(self)
        self.stack.addWidget(self.devicePage)   # index 0
        self.stack.addWidget(self.dataPage)     # index 1
        self.stack.addWidget(self.statusPage)   # index 2
        outer.addWidget(self.stack)

        self.dataPage.load_saved_credentials()
        self.statusBar().showMessage("Ready", 3000)

    def goto_page(self, index):
        self.stack.setCurrentIndex(index)

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
