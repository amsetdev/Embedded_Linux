"""Configuration from /etc/gateway, and SIGHUP reload (systemctl reload gateway)."""

import json

import pytest

from conftest import DEVICE_ID, RTU_REGISTERS, base_config

pytestmark = pytest.mark.hardware


@pytest.fixture(scope="module")
def configured(gateway):
    return gateway.apply_config(base_config([r for r, _ in RTU_REGISTERS[:3]]))


def test_settings_read_from_etc_gateway(gateway, configured):
    log = gateway.b.since(configured)
    assert "config=/etc/gateway/smart_rtu_config.json  data-dir=/var/lib/gateway" in log
    assert f"Device ID      : {DEVICE_ID}" in log
    assert "Interval       : 5 sec" in log
    assert "[DATA] Loaded 3 registers" in log


def test_ota_topics_use_configured_device_id(gateway, configured):
    """Regression (on the board): OTA topics used to be devices/AMSET-001/ota/*."""
    log = gateway.b.since(configured)
    assert f"OTA App Topic  : devices/{DEVICE_ID}/ota/app" in log
    assert "OTA Dir        : /var/lib/gateway/ota" in log
    assert "App Binary     : /opt/gateway/bin/gateway" in log


def test_wifi_left_alone(gateway, configured):
    log = gateway.b.since(configured)
    assert "WiFi Enable    : 0" in log
    assert "Applying Wi-Fi configuration" not in log


def test_no_third_party_http_post(gateway, configured):
    """Regression: every payload was also POSTed to https://httpbin.org/post."""
    gateway.b.wait_log(configured, r"\[MB\] Cycle \d+ complete", timeout=90)
    assert "httpbin" not in gateway.b.since(configured)


def test_sighup_reloads_settings_and_registers_without_restart(gateway, configured):
    pid = gateway.b.main_pid()
    cfg = base_config([r for r, _ in RTU_REGISTERS[:5]])
    cfg["device"]["interval_sec"] = 7
    gateway.b.put(json.dumps(cfg), "/etc/gateway/smart_rtu_config.json", mode=0o600)
    mark = gateway.b.mark()
    gateway.b.service("reload")
    gateway.b.wait_log(mark, r"Reload requested", timeout=30)
    gateway.b.wait_log(mark, r"Interval       : 7 sec", timeout=30)
    gateway.b.wait_log(mark, r"\[DATA\] Loaded 5 registers", timeout=120)
    assert gateway.b.main_pid() == pid
