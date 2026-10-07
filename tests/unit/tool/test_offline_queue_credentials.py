"""core/offline_queue.py (pending MQTT pushes on disk) and core/credentials.py (OS keyring).

The queue directory and the keyring are replaced by conftest.py fixtures.
"""

import json

import pytest

import core.offline_queue as q
from core import credentials
from fakes import FakeClock

pytestmark = pytest.mark.unit


def test_enqueue_list_remove(queue_dir):
    path = q.enqueue("GW-1", "csv-content", '{"device": {}}')
    assert path.startswith(str(queue_dir))
    (item,) = q.list_queued()
    assert (item.device_id, item.csv_content, item.json_content, item.filepath) == (
        "GW-1", "csv-content", '{"device": {}}', path)
    q.remove(path)
    assert q.list_queued() == []
    q.remove(path)   # removing twice is harmless


def test_corrupt_and_foreign_files_skipped(queue_dir):
    q.enqueue("GW-1", "a")
    (queue_dir / "broken.json").write_text("{not json")
    (queue_dir / "notes.txt").write_text("x")
    assert [i.device_id for i in q.list_queued()] == ["GW-1"]


def test_listed_oldest_first(queue_dir, monkeypatch):
    clock = FakeClock()
    monkeypatch.setattr(q, "time", clock)
    q.enqueue("GW-1", "first")
    clock.sleep(5)
    q.enqueue("GW-1", "second")
    assert [i.csv_content for i in q.list_queued()] == ["first", "second"]


def test_two_pushes_in_one_second_both_kept(queue_dir, monkeypatch):
    """Regression: files were named <device_id>__<unix seconds>.json, so a second push to
    the same device in the same second overwrote the first."""
    monkeypatch.setattr(q, "time", FakeClock())
    q.enqueue("GW-1", "first")
    q.enqueue("GW-1", "second")
    assert [i.csv_content for i in q.list_queued()] == ["first", "second"]


def test_queued_entry_keeps_the_json_the_firmware_reads(queue_dir):
    """Regression: only the CSV was queued and re-sent, but the firmware reads the JSON."""
    path = q.enqueue("GW-1", "csv-content", '{"device": {}}')
    assert json.loads(open(path).read())["json_content"] == '{"device": {}}'


def test_entries_from_older_tool_versions_still_listed_in_order(queue_dir, monkeypatch):
    queue_dir.mkdir(exist_ok=True)
    (queue_dir / "GW-1__1000.json").write_text(json.dumps(
        {"device_id": "GW-1", "csv_content": "old", "queued_at": 1000}))
    clock = FakeClock()
    monkeypatch.setattr(q, "time", clock)
    q.enqueue("GW-1", "new", "{}")
    items = q.list_queued()
    assert [(i.csv_content, i.json_content) for i in items] == [("old", ""), ("new", "{}")]


def test_credentials_saved_read_and_cleared(memory_keyring):
    credentials.save_mqtt_password("m-pw")
    credentials.save_ssh_password("s-pw")
    assert (credentials.get_mqtt_password(), credentials.get_ssh_password()) == ("m-pw", "s-pw")
    assert memory_keyring.store == {("smart_rtu_tool", "mqtt_password"): "m-pw",
                                    ("smart_rtu_tool", "ssh_password"): "s-pw"}
    credentials.clear_all()
    assert memory_keyring.store == {}
    credentials.clear_all()   # nothing stored: no error
