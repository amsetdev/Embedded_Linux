"""Offline buffer (/var/lib/gateway/storage): payloads that can't be published are
kept as one file each, in order, never overwritten."""

import re

import pytest

from conftest import base_config

pytestmark = pytest.mark.hardware


@pytest.fixture(scope="module")
def offline(gateway):
    gateway.apply_config(base_config([]))            # no broker: everything goes to storage
    gateway.clear_storage()
    return gateway.wait_stored(count=3, timeout=90)


def test_file_names_unix_ms_and_sequence(offline):
    """Regression: names had 1 s resolution (<s>000.txt) and could overwrite each other."""
    for name, _ in offline:
        assert re.fullmatch(r"\d{13}_\d{6}\.txt", name), name


def test_files_in_storing_order(offline):
    names = [n for n, _ in offline]
    assert names == sorted(names)
    stamps = [p["ts"] for _, p in offline]
    assert stamps == sorted(stamps)


def test_payloads_are_valid_telemetry(offline):
    for _, payload in offline:
        assert set(payload) == {"ts", "values"}
