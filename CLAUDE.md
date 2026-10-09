# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

STM32MP1 Industrial IoT Gateway — an embedded Linux application (C, ARM Cortex-A7) that reads Modbus RTU/TCP data from industrial devices, publishes to AWS IoT Core via MQTT (X.509 mutual TLS), stores data offline in SQLite, and provides a DRM-based touchscreen UI. Target board: STM32MP157F-DK2.

## Build Commands

All builds use Docker with an ARM cross-compiler (`arm-linux-gnueabihf-gcc`):

```bash
# Build the Docker image (first time)
docker build -t stm32mp1-build .

# Build the project (produces build/main)
docker run --rm -v ${PWD}:/project stm32mp1-build

# Clean build artifacts
docker run --rm -v ${PWD}:/project stm32mp1-build make clean

# Build + deploy to board via SSH
docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make flash
```

Board connection defaults are configured in `makefile` (`BOARD_IP`, `BOARD_USER`, `BOARD_DIR`).

## Architecture

### Threading Model (6 threads)

1. **Modbus RTU Thread** (`mb_thread_func` in main.c) — polls RS485 registers via fieldbus abstraction, builds JSON payload, enqueues to message queue, sleeps for configured interval
2. **MQTT Publisher Thread** (`mqtt_publisher_thread_func` in main.c) — dequeues payloads from message queue, publishes via MQTT and HTTPS (decoupled from polling)
3. **Modbus TCP Thread** (`mb_thread_func1` in mb_tcp.c) — connects to Modbus TCP slave, reads holding registers, logs to SQLite. Only starts when `modbus_tcp.enable = 1` in config.
4. **RTC Sync Thread** (`rtc_sync_thread_func` in main.c) — NTP time synchronization with retry logic
5. **Watchdog Thread** (`watchdog_thread_func` in watchdog.c) — monitors thread heartbeats, pets hardware watchdog `/dev/watchdog` only when all threads are healthy
6. **Main Thread** — touch input polling, DRM display rendering, SIGHUP config reload

Shared state between threads is protected by mutexes (e.g., `points_mutex`).

### Fieldbus Abstraction Layer

Protocol-agnostic driver interface (`fieldbus.h`) with vtable pattern:
- `fieldbus_rtu.c` — Modbus RTU driver (wraps `modbus.c` RS485/UART functions)
- `fieldbus_tcp.c` — Modbus TCP driver (wraps libmodbus TCP)
- `data.c` reads/writes registers through `fieldbus_driver_t` vtable, not direct protocol calls
- Vtable functions: `init`, `read_register`, `read_block`, `write_register`, `write_block`, `set_slave`, `close`
- To add a new protocol: implement the vtable functions in a new file

### Key Module Responsibilities

| Module | Purpose |
|---|---|
| `fieldbus.h` | Protocol-agnostic driver interface (vtable: read, write, set_slave) |
| `fieldbus_rtu.c` | Modbus RTU driver — wraps modbus.c (FC01-06, FC0F, FC10) |
| `fieldbus_tcp.c` | Modbus TCP driver — wraps libmodbus (FC01-06, FC0F, FC10) |
| `modbus.c` | RS485 UART, GPIO DE pin control (PE10 via gpiochip4), raw Modbus RTU transactions |
| `mqtt.c` | MQTT publish to AWS IoT Core (X.509 mTLS, port 8883) |
| `mb_tcp.c` | Modbus TCP polling thread with SQLite logging |
| `data.c` | Register config parsing, multi-slave polling via fieldbus driver, write API |
| `storage.c` | File-based offline message queue — stores when broker is down, replays on reconnect |
| `msg_queue.c` | Thread-safe bounded ring buffer (decouples RTU polling from MQTT publishing) |
| `settings.c` | JSON config parser for `smart_rtu_config.json`, supports SIGHUP reload |
| `watchdog.c` | Hardware watchdog + thread heartbeat monitoring |
| `display.c` | DRM framebuffer rendering (480x800), touch input |
| `connection.c` | Network status monitoring (Ethernet `end0`, Wi-Fi `wlan0`) |
| `wifi.c` | Wi-Fi config via wpa_supplicant |
| `ota.c` | Dual OTA updates (application binary + system image via swupdate) |

### MQTT Authentication (AWS IoT Core)

- X.509 mutual TLS (no username/password)
- Certificate paths configured in `smart_rtu_config.json` under `mqtt` section
- Requires: CA cert, device cert, private key, client ID
- TLS 1.2 minimum

### Multi-Slave Architecture

Each register in the config carries its own `slave_id`, `type`, and `data_type`. The RTU polling thread calls `set_slave()` before reading each register, allowing a single RS485 bus to poll multiple devices.

Config JSON format:
```json
{
  "registers": [
    { "slave_id": 1, "label": "Pressure",    "address": 100, "type": "holding", "data_type": "float32" },
    { "slave_id": 2, "label": "Flow_Rate",   "address": 200, "type": "input",   "data_type": "uint16"  },
    { "slave_id": 3, "label": "Motor_RPM",   "address": 0,   "type": "holding", "data_type": "uint16"  }
  ]
}
```

If `slave_id` is omitted from a register, it defaults to `device.slave_id` for backward compatibility.

### Smart RTU Tool (Python)

Desktop configuration tool (`tools/smart_rtu_tool/`) — PyQt5 wizard that imports Excel register lists, generates `smart_rtu_config.json`, and pushes configs to the board via MQTT, SSH, or Serial. Excel format supports columns: `Slave ID | Label | Address | Type | Data Type | Unit`.

### Configuration

All settings loaded from `smart_rtu_config.json` by `settings_load()`. Sections: `device`, `wifi`, `mqtt` (broker, certs, topic), `modbus_tcp` (enable, IP, port, slave), `watchdog` (enable, timeout), `ota` (enable, topics), `registers` (per-register slave_id, type, data_type). No credentials in source code.

### Config Hot-Reload

`SIGHUP` triggers re-read of `smart_rtu_config.json` and MQTT reconnect with new settings. Usage: `kill -SIGHUP $(pidof main)`.

### RS485 Direction Control

Modbus RTU uses GPIO PE10 (gpiochip4 line 10) for RS485 DE pin: HIGH before TX, LOW before RX, with pre-TX delay (200us) and post-TX guard (1100us).

## Linked Libraries

`-lmodbus -lmosquitto -lsqlite3 -lpthread -lssl -lcrypto -lcurl -lz -lm -ldl`

## Code Conventions

- **Functions:** snake_case (`mqtt_publish`, `uart_open`)
- **Structs:** PascalCase (`ModbusPoint`, `AppSettings`)
- **Constants:** UPPERCASE (`MAX_POINTS`, `PAYLOAD_MAX`)
- **Documentation:** Doxygen-style headers on all functions (`@brief`, `@param`, `@return`)
- **Error handling:** Return codes (0 success, 1 fail), `perror()` for system errors, graceful degradation
- **Optional features:** Gated with config flags (e.g., `modbus_tcp.enable`)
