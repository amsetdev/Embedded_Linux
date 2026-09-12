# Smart RTU Tool — Usage Guide

## Prerequisites

- Python 3.9+ (tested on 3.10–3.12)
- pip dependencies: `pip install -r requirements.txt`
- For MQTT mode: a HiveMQ Cloud (or compatible) broker with credentials
- For SSH mode: the board must be reachable on the same LAN
- For Serial mode: a USB cable connected to the board's console port (not the RS485/Modbus adapter)

## Quick Start

```bash
cd tools/smart_rtu_tool
python -m venv venv
source venv/bin/activate        # Windows: venv\Scripts\activate
pip install -r requirements.txt
python run.py
```

Or, if using the frozen `.exe` build: just double-click `Smart_RTU_Tool.exe`.

## Walkthrough

The tool is a 3-page wizard. Navigate using the top nav bar or the Next/Back buttons.

### Page 1 — Device Config

1. **Wi-Fi Settings** — enter the SSID and password of the network the board should connect to. Leave blank if the board uses a USB/cellular dongle or wired Ethernet.

2. **Device Settings**:
   - **Device Name / ID** — a friendly identifier (e.g. `AMSET-001`). Also used as the MQTT topic key, so it must match the `device_id` configured on the board.
   - **Modbus Slave ID** — the RS485 slave address (1–247).
   - **Baud Rate** — the RS485 field baud rate (typically 9600).
   - **Poll Interval** — how often the board reads its Modbus registers (seconds).

3. **Modbus Map Sizing** — sets how the register table rows (on Page 2) map to register types:
   - **Holding Integers / Decimals / Double Integers** — how many of the first N rows are holding registers of each data type (word, float32, int32).
   - **Input Integers / Decimals / Double Integers** — same for input registers, following the holding rows.
   - **Coils / Alerts** — digital output and alarm point counts.
   - **Parity / Stop Bits** — serial line parameters.

   The order rule: rows in the Data Transmission table are assigned types top-to-bottom in the order listed above.

4. **Read / Send / Erase Settings** — these buttons let you read back, push, or erase the combined config file directly from Page 1 using whichever delivery method is selected on Page 2. The config sent is identical to what "Push Data to Device" sends — Wi-Fi + Device Settings + Modbus Map Sizing + Registers, all in one file.

### Page 2 — Data Transmission

1. **Registers Table** — the register list to deploy to the board:
   - **Import from Excel** — loads a `.xlsx` file. Only Label and Address columns are required (the importer matches common column name variants like "Name", "Tag", "Addr", "Register"). Bad rows are skipped with warnings.
   - **Add Row Manually** — appends a blank row and drops you into editing.
   - **Delete Selected** — removes highlighted rows.
   - The live point counter shows how many rows are configured.

2. **Validate** — checks for empty labels, out-of-range addresses (0–65535), and duplicate register addresses. Fix any errors before pushing.

3. **Save / Load Project** — saves the full configuration (Wi-Fi, device settings, sizing, registers) as a `.json` project file for reuse. Load it back on a different PC or session.

4. **Export Full Config** — writes both `.csv` and `.json` config files to disk without sending them to the device. Useful for manual transfer via USB stick.

5. **Delivery Method** — choose how configs reach the board:

   | Tab | When to use |
   |-----|-------------|
   | **MQTT (Field / Dongle)** | Board is behind a USB/cellular dongle or NAT. Enter HiveMQ broker host, port (8883 for TLS), username, password. |
   | **SSH (LAN)** | Board has a reachable IP on the same LAN. Enter board IP, SSH username, password. |
   | **Serial / USB** | No network at all. Plug a USB cable into the board's console port. Select the serial port and console baud (usually 115200). |

   Check "Remember password" to store credentials in the OS keyring (Windows Credential Manager / macOS Keychain / Linux Secret Service).

6. **Test Connection** — verifies that the selected delivery method can reach the target before pushing.

7. **Refresh Ports** — re-scans USB serial ports for the Serial/USB tab.

8. **Push Data to Device** — validates, then sends the combined config:
   - **MQTT**: publishes retained (QoS 1), waits up to 15s for the board's acknowledgment. If the board is offline, the config is queued locally.
   - **SSH**: uploads via SFTP, then restarts `./main` on the board.
   - **Serial**: writes via heredoc over the console session.

9. **Retry Pending Queue** — retries any configs that were queued locally because the MQTT broker or board was unreachable at push time.

### Page 3 — Status

- **Delivery Method** — shows which transport was last used.
- **Device Config Used** — recap of Wi-Fi, device settings, sizing, and register count from the last push.
- **Status Pill** — green (success), red (attention needed), yellow (working), or grey (idle).
- **Full Log** — timestamped log of all actions, warnings, and errors.
- **Erase All Data** — permanently deletes all locally stored data (saved credentials + pending retry queue). Does not affect data on the device.
- **Close Tool** — appears after a successful push.

## Building the Frozen .exe

```bash
pip install pyinstaller
pyinstaller Smart_RTU_Tool.spec
```

Output: `dist/Smart_RTU_Tool/`. If `keyring` silently returns `None` in the frozen build, uncomment the platform-specific backend in the `.spec` file's `hiddenimports` and rebuild.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "No ports found" on Serial tab | Plug in the USB cable, click Refresh Ports. Make sure it's the console port, not the RS485 adapter. |
| MQTT push succeeds but no ack | The board may be offline or hasn't been updated with the MQTT subscribe handler (see `FIRMWARE_NOTES.md`). Config is retained on the broker and will apply when the board reconnects. |
| SSH connection refused | Verify the board's IP is correct and SSH is running (`systemctl status sshd` on the board). |
| Import from Excel finds 0 rows | Check that the spreadsheet has columns named Label (or Name/Tag) and Address (or Addr/Register). |
| Keyring errors on Linux | Install `gnome-keyring` or `kwallet` and ensure the Secret Service D-Bus interface is available. |
