"""
offline_queue.py
-----------------
When a field technician configures a device but the dongle/broker is
unreachable at that moment, we must not lose the configuration. This
queues the (device_id, csv_content) pair to local disk and lets the
tool retry later (e.g. on next launch, or via a "Retry Pending" button).
"""

from __future__ import annotations
import os
import json
import time
from dataclasses import dataclass
from typing import List


QUEUE_DIR = os.path.join(os.path.expanduser("~"), ".smart_rtu_tool", "pending_configs")


@dataclass
class QueuedConfig:
    filepath: str
    device_id: str
    csv_content: str
    queued_at: float


def _ensure_dir():
    os.makedirs(QUEUE_DIR, exist_ok=True)


def enqueue(device_id: str, csv_content: str) -> str:
    _ensure_dir()
    ts = int(time.time())
    fname = f"{device_id}__{ts}.json"
    filepath = os.path.join(QUEUE_DIR, fname)
    with open(filepath, "w") as f:
        json.dump({
            "device_id": device_id,
            "csv_content": csv_content,
            "queued_at": ts,
        }, f)
    return filepath


def list_queued() -> List[QueuedConfig]:
    _ensure_dir()
    items = []
    for fname in sorted(os.listdir(QUEUE_DIR)):
        if not fname.endswith(".json"):
            continue
        filepath = os.path.join(QUEUE_DIR, fname)
        try:
            with open(filepath) as f:
                d = json.load(f)
            items.append(QueuedConfig(
                filepath=filepath,
                device_id=d["device_id"],
                csv_content=d["csv_content"],
                queued_at=d["queued_at"],
            ))
        except Exception:
            continue  # skip corrupt entries rather than crash the UI
    return items


def remove(filepath: str):
    try:
        os.remove(filepath)
    except FileNotFoundError:
        pass
