"""The PyQt5 window (ui/main_window.py), headless: what the user's clicks do.

Dialogs are answered through the `dialogs` fixture (conftest.py); network
workers are replaced so nothing is sent anywhere.
"""

import json

import pytest
from PyQt5.QtWidgets import QMessageBox
from openpyxl import Workbook

import ui.main_window as mw
from core import REMOTE_CA_CERT_PATH, offline_queue, credentials
from core.register_model import DeviceConfig, RegisterPoint

pytestmark = pytest.mark.unit


def fill_device_page(w):
    p = w.devicePage
    p.device_id_edit.setText("GW-9")
    p.slave_id_spin.setValue(4)
    p.baud_combo.setCurrentText("19200")
    p.parity_combo.setCurrentText("Even")
    p.stop_bits_combo.setCurrentText("2")
    p.interval_spin.setValue(15)
    p.wifi_ssid_edit.setText("plant")
    p.wifi_pass_edit.setText("pw")
    p.tcp_enable_chk.setChecked(True)
    p.tcp_ip_edit.setText("10.0.0.5")


def test_window_starts_on_device_page(window):
    assert window.stack.currentIndex() == 0
    assert [b.isChecked() for b in window.nav_buttons] == [True, False, False, False]


def test_device_page_to_config_and_back(window):
    fill_device_page(window)
    config = window.devicePage.apply_to_config(DeviceConfig())
    assert (config.device_id, config.slave_id, config.baud, config.parity, config.stop_bits,
            config.interval_sec, config.modbus_tcp_enable, config.modbus_tcp_ip) == (
        "GW-9", 4, 19200, "Even", 2, 15, 1, "10.0.0.5")
    window.devicePage.device_id_edit.setText("other")
    window.devicePage.load_from_config(config)
    assert window.devicePage.apply_to_config(DeviceConfig()) == config


def test_empty_device_id_falls_back_to_default(window):
    window.devicePage.device_id_edit.setText("   ")
    assert window.devicePage.apply_to_config(DeviceConfig()).device_id == "AMSET-001"


def test_mqtt_page_always_writes_board_cert_paths(window, dialogs, tmp_path):
    ca = tmp_path / "AmazonRootCA1.pem"
    ca.write_text("PEM")
    dialogs.open_path = str(ca)
    window.mqttPage._browse_cert("ca", "")
    config = window.mqttPage.apply_to_config(DeviceConfig())
    assert config.mqtt_ca_cert == REMOTE_CA_CERT_PATH     # not the PC path
    assert window.mqttPage.get_local_cert_paths() == [(str(ca), REMOTE_CA_CERT_PATH)]


def test_table_rows_to_points(window):
    page = window.dataPage
    page._add_row(2, "FLOW", 100, "input", "float32")
    page._add_row(0, "LEVEL", 7)
    assert page.table_to_points() == [
        RegisterPoint("FLOW", 100, slave_id=2, register_type="input", data_type="float32"),
        RegisterPoint("LEVEL", 7)]
    assert page.point_count_label.text() == "2 points configured"


def test_non_numeric_address_fails_validation_with_dialog(window, dialogs):
    window.dataPage._add_row(1, "BAD", 0)
    window.dataPage.table.item(0, 2).setText("forty")
    assert window.dataPage._validate() is False
    assert dialogs.kinds() == ["warning"] and "out of range" in dialogs.texts()[0]


def test_push_refused_when_invalid(window, dialogs, monkeypatch):
    started = []
    monkeypatch.setattr(mw.PushWorker, "start", lambda self: started.append(self))
    window.dataPage._add_row(1, "", 0)
    window.dataPage._push_config()
    assert started == [] and dialogs.kinds() == ["warning"]


def test_push_uses_selected_mode_and_full_config(window, monkeypatch):
    started = []
    monkeypatch.setattr(mw.PushWorker, "start", lambda self: started.append(self))
    fill_device_page(window)
    window.dataPage._add_row(1, "A", 0)
    window.dataPage.conn_tabs.setCurrentIndex(1)       # SSH
    window.dataPage.ssh_host_edit.setText("192.0.2.10")
    window.dataPage._push_config()
    (worker,) = started
    assert worker.mode == "ssh" and worker.conn_params["host"] == "192.0.2.10"
    assert worker.config.device_id == "GW-9" and [p.label for p in worker.config.points] == ["A"]
    assert window.stack.currentIndex() == 3            # jumps to the Status page
    window.dataPage.progress.close()


def test_remembered_passwords_go_to_keyring_only_when_ticked(window, memory_keyring, monkeypatch):
    monkeypatch.setattr(mw.PushWorker, "start", lambda self: None)
    window.dataPage._add_row(1, "A", 0)
    window.dataPage.mqtt_pass_edit.setText("mqtt-secret")
    window.dataPage.ssh_pass_edit.setText("ssh-secret")
    window.dataPage.ssh_remember_chk.setChecked(True)
    window.dataPage._push_config()
    assert memory_keyring.store == {("smart_rtu_tool", "ssh_password"): "ssh-secret"}
    window.dataPage.progress.close()


def test_saved_passwords_prefilled_on_start(memory_keyring, qapp, dialogs):
    credentials.save_mqtt_password("m")
    w = mw.MainWindow()
    try:
        assert w.dataPage.mqtt_pass_edit.text() == "m" and w.dataPage.mqtt_remember_chk.isChecked()
        assert w.dataPage.ssh_pass_edit.text() == ""
    finally:
        w.close()


def test_save_and_load_project(window, dialogs, tmp_path):
    fill_device_page(window)
    window.dataPage._add_row(3, "FLOW", 10, "holding", "float32")
    dialogs.save_path = str(tmp_path / "proj.json")
    window.dataPage._save_project()
    saved = DeviceConfig.load_json(tmp_path / "proj.json")
    assert saved.device_id == "GW-9" and saved.points[0].label == "FLOW"

    window.dataPage.table.setRowCount(0)
    window.devicePage.device_id_edit.setText("x")
    dialogs.open_path = str(tmp_path / "proj.json")
    window.dataPage._load_project()
    assert window.devicePage.device_id_edit.text() == "GW-9"
    assert window.dataPage.table_to_points() == saved.points


def test_load_broken_project_shows_error(window, dialogs, tmp_path):
    (tmp_path / "bad.json").write_text("{")
    dialogs.open_path = str(tmp_path / "bad.json")
    window.dataPage._load_project()
    assert dialogs.kinds()[-1] == "critical"


def test_cancelled_dialogs_change_nothing(window, dialogs):
    window.dataPage._add_row(1, "A", 0)
    window.dataPage._load_project()
    window.dataPage._import_excel()
    assert window.dataPage.table.rowCount() == 1
    assert dialogs.kinds() == ["open", "open"]


def test_export_writes_both_files_whatever_extension_chosen(window, dialogs, tmp_path):
    window.dataPage._add_row(1, "A", 0)
    dialogs.save_path = str(tmp_path / "plant.csv")
    window.dataPage._export_csv()
    data = json.loads((tmp_path / "plant.json").read_text())
    assert data["registers"][0]["label"] == "A"
    assert (tmp_path / "plant.csv").read_text().startswith("[DEVICE]")


@pytest.mark.parametrize("answer, expected_rows", [(QMessageBox.Yes, ["NEW"]), (QMessageBox.No, ["OLD", "NEW"])])
def test_excel_import_replace_or_append(window, dialogs, tmp_path, answer, expected_rows):
    wb = Workbook()
    wb.active.append(["Label", "Address"])
    wb.active.append(["NEW", 5])
    wb.save(tmp_path / "r.xlsx")
    window.dataPage._add_row(1, "OLD", 0)
    dialogs.open_path, dialogs.answer = str(tmp_path / "r.xlsx"), answer
    window.dataPage._import_excel()
    assert [p.label for p in window.dataPage.table_to_points()] == expected_rows


def test_excel_import_cancel_keeps_table(window, dialogs, tmp_path):
    wb = Workbook()
    wb.active.append(["Label", "Address"])
    wb.active.append(["NEW", 5])
    wb.save(tmp_path / "r.xlsx")
    window.dataPage._add_row(1, "OLD", 0)
    dialogs.open_path, dialogs.answer = str(tmp_path / "r.xlsx"), QMessageBox.Cancel
    window.dataPage._import_excel()
    assert [p.label for p in window.dataPage.table_to_points()] == ["OLD"]


def test_delete_selected_rows(window):
    for label in "ABC":
        window.dataPage._add_row(1, label, 0)
    window.dataPage.table.selectRow(1)
    window.dataPage._delete_selected_rows()
    assert [p.label for p in window.dataPage.table_to_points()] == ["A", "C"]


def test_retry_queue_needs_mqtt_tab(window, dialogs):
    offline_queue.enqueue("GW-1", "csv")
    window.dataPage.conn_tabs.setCurrentIndex(1)
    window.dataPage._retry_queue()
    assert "switch to the MQTT tab" in dialogs.texts()[-1]


@pytest.mark.parametrize("answer, erased", [(QMessageBox.Yes, True), (QMessageBox.No, False)])
def test_erase_all_local_data_asks_first(window, dialogs, memory_keyring, answer, erased):
    offline_queue.enqueue("GW-1", "csv")
    credentials.save_ssh_password("pw")
    dialogs.answer = answer
    window.statusPage._erase_all_data()
    assert (offline_queue.list_queued() == []) is erased
    assert (memory_keyring.store == {}) is erased
    assert "does NOT affect data already on the device" in dialogs.texts()[0]


def test_status_log_escapes_html(window):
    window.log("<b>not bold</b>", "error")
    assert "&lt;b&gt;not bold&lt;/b&gt;" in window.statusPage.log_view.toHtml()
