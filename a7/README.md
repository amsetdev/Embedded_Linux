# STM32MP157 Industrial IoT Gateway

Industrial IoT gateway firmware and applications for the STM32MP157 dual-core SoC (Cortex-A7 + Cortex-M4).

## Architecture

```
Field Devices (RS-485 Modbus RTU)
        │
        ▼
┌──────────────────────────────────────┐
│  Cortex-M4 (209 MHz, FreeRTOS)      │
│  - Modbus RTU Master (USART3)       │
│  - RTC Timestamping                  │
│  - Hardware Watchdog (IWDG)          │
│  - Heartbeat Signal                  │
├──────── RPMsg/OpenAMP ───────────────┤
│  Cortex-A7 (650 MHz, Linux)         │
│  - Data Broker (RPMsg → SQLite)     │
│  - MQTT Publisher (store & forward)  │
│  - Display Manager (LVGL, /dev/fb0) │
│  - Net Monitor (connectivity WDG)   │
│  - Modbus TCP Client (libmodbus)    │
└──────────────────────────────────────┘
        │
        ▼
    Cloud (MQTT over TLS)
```

## Directory Structure

```
├── m4-firmware/          Cortex-M4 FreeRTOS firmware
│   ├── Core/Inc/         Header files
│   ├── Core/Src/         Source files
│   ├── OpenAMP/          Resource table for IPC
│   ├── Linker/           Linker script
│   └── Makefile
├── linux-apps/           Cortex-A7 Linux applications
│   ├── common/           Shared library (config, DB, protocol, CRC)
│   ├── data-broker/      RPMsg reader → SQLite router
│   ├── mqtt-publisher/   Store-and-forward MQTT client
│   ├── display-manager/  LVGL TFT dashboard
│   ├── net-monitor/      Network watchdog
│   ├── modbus-tcp/       Modbus TCP client library
│   └── CMakeLists.txt
├── config/               Runtime configuration (JSON)
├── systemd/              Service unit files
├── yocto/                Yocto meta-gateway layer
├── scripts/              Deployment and flash scripts
└── DOCS/                 Architecture documentation
```

## Building

### M4 Firmware

Requires `arm-none-eabi-gcc` toolchain.

```bash
cd m4-firmware
make
# Output: build/m4-firmware.elf
```

### Linux Applications

Requires a Yocto SDK or cross-compilation toolchain with:
- SQLite3
- cJSON
- Paho MQTT C
- LVGL
- libmodbus

```bash
mkdir -p linux-apps/build && cd linux-apps/build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake
make -j$(nproc)
```

### Full Image (Yocto)

```bash
source oe-init-build-env
bitbake-layers add-layer ../meta-gateway
bitbake gateway m4-firmware
```

## Deployment

```bash
# Deploy Linux apps + configs to target
./scripts/deploy.sh 192.168.1.100

# Flash M4 firmware
./scripts/flash-m4.sh 192.168.1.100
```

## Configuration

All runtime config is in `/opt/gateway/etc/`:

| File | Purpose |
|------|---------|
| `config.json` | MQTT broker, database, logging, network |
| `modbus-map.json` | Slave addresses, registers, data types |
| `display-layout.json` | TFT dashboard layout |

## Data Flow

1. **M4** polls Modbus RTU slaves via RS-485
2. **M4** timestamps readings (RTC) and sends via RPMsg
3. **Data Broker** reads RPMsg, validates CRC, inserts into SQLite
4. **MQTT Publisher** drains SQLite queue to cloud (QoS 1)
5. **Display Manager** shows live data on TFT via LVGL

## Reliability

- **Zero data loss**: SQLite WAL + QoS 1 PUBACK tracking
- **Offline capacity**: 18 days at full poll rate (4 GB storage)
- **Auto-recovery**: Exponential backoff reconnect, hardware watchdog
- **Dual-core watchdog**: M4 kicks IWDG; if either core hangs, board resets

## Documentation

See `DOCS/` for detailed architecture documents:
- 01: Project Overview
- 02: System Architecture
- 03: Cortex-M4 Firmware
- 04: Cortex-A7 Linux
- 05: OpenAMP/RPMsg IPC
- 06: Modbus Integration
- 07: MQTT Cloud
- 08: Data Retention
# Embedded_Linux
# Embedded_Linux
