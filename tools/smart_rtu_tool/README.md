# Smart RTU Tool — STM32MP157F-DK2 Edition

Desktop configuration tool for the Modbus RTU reader running on the
STM32MP157F-DK2 (Linux, `parse_csv()`/`data.c` firmware). Builds a
`registers.csv` from a table UI and delivers it to the board over
**MQTT** (recommended for field/dongle deployments) or **SSH/SCP**
(LAN/bench only).

## Project layout

```
Smart_RTU_Tool_Linux/
├── run.py                   entry point
├── requirements.txt
├── Smart_RTU_Tool.spec      PyInstaller build config
├── FIRMWARE_NOTES.md        required board-side C changes for MQTT mode
├── core/
│   ├── register_model.py    RegisterPoint / DeviceConfig, CSV + JSON I/O
│   ├── ssh_transport.py     SFTP push + remote restart (LAN fallback)
│   ├── mqtt_transport.py    MQTT config push + ack wait (primary, field)
│   ├── offline_queue.py     local retry queue for failed MQTT pushes
│   ├── credentials.py       OS keyring wrapper (no plaintext passwords)
│   └── excel_import.py      flexible .xlsx register-list import
└── ui/
    └── main_window.py       PyQt5 GUI
```

## Setup (running from source)

```bash
python -m venv venv
source venv/bin/activate        # Windows: venv\Scripts\activate
pip install -r requirements.txt
python run.py
```

## Using the tool

1. **Device panel** — set Device ID (must match `device_id` in the
   board's `settings.config` for MQTT mode), Slave ID, baud, poll
   interval.
2. **Registers table** — add points manually (`Add Row`, which drops
   you straight into editing the new row), in bulk (`Add Range…` for
   sequential Word_1..Word_N style test sets), import from a `.xlsx`
   register list (`Import from Excel…`), or reload a previously saved
   project (`Load Project…`). The live point counter under the table
   tracks how many rows are currently configured.

   **Excel import column matching** is flexible — only a Label
   (or Name/Point/Tag) and Address (or Addr/Register) column are
   required; Register Type, Unit, Data Type, and Scale columns are
   optional and default sensibly if omitted or misspelled. Bad rows
   (empty label, non-numeric address) are skipped with a warning
   shown in the Status log rather than aborting the whole import.
3. **Validate** — checks for empty labels, out-of-range addresses,
   and duplicate (register_type, address) pairs before anything is
   sent.
4. **Connection tab**:
   - **MQTT (Field / Dongle)** — enter your HiveMQ broker host, port
     (8883 for TLS), username/password. This is the path that works
     behind a USB/cellular dongle with no fixed/reachable IP.
   - **SSH (LAN Only)** — enter the board's IP + SSH credentials.
     Only works when your PC can reach that IP directly.
   Check "Remember password" to store it in the OS keyring instead of
   typing it every session.
5. **Test Connection** — verify credentials/reachability before
   pushing.
6. **Push Config to Device** — for MQTT mode, publishes the CSV
   (retained, QoS 1) and waits up to 15s for the board's ack. If the
   board is offline, the config is queued locally and the push isn't
   marked as a hard failure — use **Retry Pending Queue** once the
   board is back online. For SSH mode, pushes the CSV then restarts
   `./main` on the board.
7. **Export registers.csv…** — save the CSV locally without pushing,
   e.g. to hand-carry via USB stick if no network path is available
   on-site at all.

## Firmware requirement for MQTT mode

MQTT push requires the board to subscribe to its config topic and
apply changes — this is **not** in your current firmware and must be
added. See `FIRMWARE_NOTES.md` for the exact C changes
(`mqtt.c` subscribe handler + `main.c` reload-in-place logic).
SSH mode works against your existing firmware with no changes.

## Building the frozen .exe

```bash
pip install pyinstaller
pyinstaller Smart_RTU_Tool.spec
```

Output lands in `dist/Smart_RTU_Tool/`. If `keyring.get_password()`
silently returns `None` in the frozen build even though it works when
running from source, uncomment the matching backend line
(`keyring.backends.Windows` / `.macOS` / `.SecretService`) in the
`.spec` file's hiddenimports and rebuild — PyInstaller sometimes misses
keyring's platform backend during auto-detection.

## Known limitations (matches current firmware capability)

- Every register is currently read/written as a single 16-bit word.
  The tool's `data_type` field (int32/float32) is captured and saved
  for forward compatibility but **not yet acted on** by
  `mb_transaction()`/`parse_csv()` — extending those to combine two
  registers into a 32-bit value is a separate firmware change.
- MQTT ack confirms the board *applied* the config, not that every
  individual register subsequently reads successfully — that's still
  visible in the board's own `[MB] Cycle N done — X/Y ok` log output.
