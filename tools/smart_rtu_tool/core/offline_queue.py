"""
offline_queue.py
-----------------
When a field technician configures a device but the dongle/broker is
unreachable at that moment, we must not lose the configuration. This
queues the device's config (the JSON the firmware reads, plus the CSV copy)
to local disk and lets the tool retry later (e.g. on next launch, or via a
"Retry Pending" button). Entries are listed oldest first.
"""

from __future__ import annotations
import os
import json
import time
import itertools
import uuid
from dataclasses import dataclass
from typing import List


_counter = itertools.count()

QUEUE_DIR = os.path.join(os.path.expanduser("~"), ".smart_rtu_tool", "pending_configs")


@dataclass
class QueuedConfig:
    filepath: str
    device_id: str
    csv_content: str
    queued_at: float
    json_content: str = ""      # empty for entries queued by older tool versions


def _ensure_dir():
    os.makedirs(QUEUE_DIR, exist_ok=True)


def enqueue(device_id: str, csv_content: str, json_content: str = "") -> str:
    _ensure_dir()
    ts = time.time()
    # Nanosecond timestamp, then a per-process counter (sort order = queue order),
    # then a random part: two pushes in the same second never share a file name.
    fname = f"{int(ts * 1e9):020d}_{next(_counter):06d}_{uuid.uuid4().hex[:8]}.json"
    filepath = os.path.join(QUEUE_DIR, fname)
    with open(filepath, "x") as f:
        json.dump({
            "device_id": device_id,
            "csv_content": csv_content,
            "json_content": json_content,
            "queued_at": ts,
        }, f)
    return filepath


def list_queued() -> List[QueuedConfig]:
    _ensure_dir()
    items = []
    def order(fname):
        # older tool versions wrote "<device>__<unix s>.json": order those by time too
        stem = fname[:-5]
        if "__" in stem and stem.rsplit("__", 1)[1].isdigit():
            return f"{int(stem.rsplit('__', 1)[1]) * 10**9:020d}"
        return stem

    for fname in sorted(os.listdir(QUEUE_DIR), key=order):
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
                json_content=d.get("json_content", ""),
            ))
        except Exception:
            continue  # skip corrupt entries rather than crash the UI
    return items


def remove(filepath: str):
    try:
        os.remove(filepath)
    except FileNotFoundError:
        pass
