# Smart RTU Tool — Architecture

## Module Map

```
smart_rtu_tool/
├── run.py                     Entry point — launches the Qt GUI
├── requirements.txt           Python dependencies
├── Smart_RTU_Tool.spec        PyInstaller config for frozen .exe builds
├── FIRMWARE_NOTES.md          Board-side C changes needed for MQTT mode
├── ARCHITECTURE.md            This file
├── USAGE.md                   Step-by-step user guide
│
├── core/                      Business logic (no Qt dependency)
│   ├── __init__.py            Shared constants (remote paths, app dir)
│   ├── transport.py           Transport ABC + TransportResult dataclass
│   ├── ssh_transport.py       SSH/SFTP delivery (LAN fallback)
│   ├── mqtt_transport.py      MQTT delivery (primary, field/dongle)
│   ├── serial_transport.py    USB-serial console delivery (no-network fallback)
│   ├── register_model.py      RegisterPoint, DeviceConfig, CSV/JSON I/O
│   ├── excel_import.py        .xlsx register-list import with flexible column matching
│   ├── offline_queue.py       Local disk queue for failed MQTT pushes
│   └── credentials.py         OS keyring wrapper (no plaintext passwords)
│
└── ui/                        PyQt5 GUI layer
    ├── __init__.py
    └── main_window.py         3-page wizard (Device Config, Data Transmission, Status)
```

## Data Flow

```
Excel (.xlsx)  ──┐
                 ├──► RegisterPoint[] ──► DeviceConfig
Manual entry  ──┘         │                    │
                          │         ┌──────────┴──────────┐
                          │         ▼                      ▼
                   assign_types    to_full_config_json()  to_full_config_csv_string()
                   _from_sizing()        │                      │
                          │              └──────┬───────────────┘
                          ▼                     ▼
                   typed points        csv_content + json_content
                                                │
                              ┌─────────────────┼─────────────────┐
                              ▼                 ▼                 ▼
                        MQTTTransport    SSHTransport      SerialTransport
                              │                 │                 │
                              ▼                 ▼                 ▼
                        HiveMQ broker     Board via SFTP   Board via console
                              │                 │                 │
                              └─────────────────┴─────────────────┘
                                                │
                                                ▼
                              /home/root/edb_c/linking/
                                smart_rtu_config.csv
                                smart_rtu_config.json
```

## Transport Abstraction

All three delivery backends inherit from `core.transport.Transport` (ABC) and return `TransportResult`:

| Method | Purpose |
|--------|---------|
| `test_connection()` | Verify reachability |
| `push_text(content, remote_path)` | Write arbitrary text to device |
| `push_combined_config(csv, json)` | Push both config formats in one call |
| `read_text(remote_path)` | Read a file / retained message back |
| `delete_remote(remote_path)` | Erase a file / clear a retained message |

Transport-specific extras (MQTT topics, serial login credentials) pass through `**kwargs`.

`_make_transport(mode, conn_params)` in `main_window.py` is the factory that instantiates the right backend from the UI's connection-tab state.

## Threading Model

The UI runs on Qt's main thread. All network I/O happens in `QThread` workers to keep the GUI responsive:

| Worker | Launched by | Purpose |
|--------|-------------|---------|
| `PushWorker` | "Push Data to Device" button | Pushes combined config via selected transport |
| `ConfigFieldWorker` | Read/Send/Erase buttons on Device Config page | Runs a single read/send/erase operation |
| `TestWorker` | "Test Connection" button | Calls `transport.test_connection()` |
| `RetryQueueWorker` | "Retry Pending Queue" button | Drains offline queue over MQTT |

Workers emit signals (`finished`, `finished_ok`, `finished_fail`) back to the main thread for UI updates.

## Configuration & Credential Storage

- **Device config** (registers, Wi-Fi, Modbus sizing) is held in `DeviceConfig` and serialized to a combined `.csv` + `.json` file delivered to the board.
- **Project files** (`.json`) save/load the full `DeviceConfig` for reuse across sessions via `Save Project` / `Load Project`.
- **MQTT/SSH passwords** are stored in the OS keyring via `core.credentials` (Windows Credential Manager / macOS Keychain / Linux Secret Service). Never written to disk as plaintext.
- **Failed MQTT pushes** are queued to `~/.smart_rtu_tool/pending_configs/` as JSON files by `core.offline_queue` and retried on demand.

## Design Decisions

1. **One combined config file** — Wi-Fi, device settings, Modbus map sizing, and registers are all delivered as a single `.csv` + `.json` pair instead of three separate files. Simplifies both the tool and firmware.
2. **MQTT as primary transport** — works behind NAT/CGNAT dongles where the board has no reachable IP. SSH and serial are fallbacks for bench/LAN and no-network scenarios.
3. **Modbus map sizing drives type assignment** — register type and data type are not entered per-row; instead, the counts on Device Config (Holding Integers/Decimals/Double Integers, Input equivalents) determine how table rows are typed positionally via `assign_types_from_sizing()`.
4. **No firmware dependency for SSH/serial** — SSH and serial modes work with the existing firmware. Only MQTT mode requires board-side subscribe+ack logic (see `FIRMWARE_NOTES.md`).
