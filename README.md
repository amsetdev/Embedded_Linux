# STM32MP1 Industrial IoT Gateway (Cortex-A7)

Modbus RTU/TCP data acquisition and MQTT cloud publishing for STM32MP157F-DK2.

## Prerequisites

- [Docker Desktop](https://www.docker.com/products/docker-desktop/) installed and running
- Git

No other toolchain, compiler, or library installation is needed. Everything runs inside the Docker container.

## Quick Start

```bash
# 1. Clone the repo
git clone <repo-url>
cd Embedded_Linux

# 2. Build the Docker image (one-time)
docker build -t stm32mp1-build .

# 3. Build the project (binary + shared libraries)
docker run --rm -v ${PWD}:/project stm32mp1-build
```

The ARM binary is output at `build/main` and all required shared libraries are collected into `build/lib/`.

## Build Commands

| Action | Command |
|--------|---------|
| Build | `docker run --rm -v ${PWD}:/project stm32mp1-build` |
| Build + Deploy (full) | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make flash` |
| Deploy binary only | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make deploy` |
| Deploy libraries only | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make deploy-libs` |
| Clean | `docker run --rm -v ${PWD}:/project stm32mp1-build make clean` |
| Shell | `docker run --rm -it -v ${PWD}:/project stm32mp1-build bash` |

Replace `<password>` with the board's root SSH password.

**Note (Windows):** If `${PWD}` doesn't mount correctly in Git Bash, use `$(pwd -W)` instead. In PowerShell, `${PWD}` works as-is.

## Deploy Configuration

The board connection is configured in `makefile`:

```makefile
BOARD_USER := root
BOARD_IP   := 192.168.0.103
BOARD_DIR  := /home/root/edb_c/linking/
```

Update `BOARD_IP` to match your board's IP address. You can also override at runtime:

```bash
docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make deploy BOARD_IP=<your-board-ip>
```

## Shared Library Deployment

The board runs a minimal OpenSTLinux (Yocto-based) image that does not include libraries like libmodbus or libmosquitto. The build system handles this automatically:

1. **`make all`** (or the default Docker CMD) compiles the binary and then runs `collect-libs`, which uses `scripts/collect-libs.sh` to recursively resolve every shared library dependency from the Docker cross-compilation sysroot and copies them into `build/lib/`.

2. **`make deploy-libs`** copies all `.so` files from `build/lib/` to `/usr/lib` on the board and runs `ldconfig` to update the linker cache.

3. **`make flash`** does everything in one shot: clean, build, deploy libraries, deploy binary.

Libraries only need to be deployed once (or when you update the Dockerfile / add new library dependencies). After the initial `deploy-libs`, subsequent deploys can use `make deploy` to push just the binary.

## Running on the Board

```bash
# SSH into the board
ssh root@<board-ip>

# Run the binary
cd /home/root/edb_c/linking/
./main

# Run in background
nohup ./main > main.log 2>&1 &

# Check if running
ps aux | grep main

# Stop
killall main
```

## Project Structure

```
Embedded_Linux/
├── src/
│   ├── main.c                  Entry point, Modbus RTU thread, RTC sync thread
│   ├── fieldbus/
│   │   ├── modbus.c            Modbus RTU (RS485/UART, GPIO DE pin control)
│   │   ├── fieldbus_rtu.c      Fieldbus driver — Modbus RTU
│   │   ├── fieldbus_tcp.c      Fieldbus driver — Modbus TCP (wraps libmodbus)
│   │   ├── data.c              Register polling via fieldbus vtable
│   │   └── mb_tcp.c            Modbus TCP polling thread + SQLite logging
│   ├── cloud/
│   │   ├── mqtt.c              MQTT publish to AWS IoT Core (X.509 mTLS)
│   │   ├── https.c             HTTPS client
│   │   ├── storage.c           Offline store-and-forward (file-based queue)
│   │   └── ota.c               OTA firmware update
│   ├── system/
│   │   ├── connection.c        Network status monitor (Ethernet/Wi-Fi)
│   │   ├── wifi.c              Wi-Fi config via wpa_supplicant
│   │   └── rtc.c               NTP time synchronization
│   ├── ui/
│   │   └── display.c           DRM framebuffer rendering (480x800) + touch input
│   └── util/
│       ├── json.c              JSON helpers
│       ├── settings.c          Config parser (smart_rtu_config.json, SIGHUP reload)
│       └── drive_logger.c      Drive log collection and upload
├── inc/                        Header files
├── scripts/
│   └── collect-libs.sh         Recursive shared library dependency collector
├── tools/
│   └── smart_rtu_tool/         PC-side config tool (PyQt5) — SSH/Serial/MQTT delivery
├── build/                      Compiled output (generated)
│   ├── main                    ARM binary
│   └── lib/                    Collected armhf shared libraries for deployment
├── Dockerfile                  Cross-compilation environment (Ubuntu 22.04, armhf)
├── makefile                    Build, deploy, deploy-libs, flash targets
└── CLAUDE.md                   AI assistant project instructions
```

## Docker Environment

The Dockerfile provides a reproducible build environment based on Ubuntu 22.04 with:

- `arm-linux-gnueabihf-gcc` cross-compiler
- ARM (`armhf`) libraries: libmodbus, libmosquitto, libsqlite3, libssl, libcurl, zlib
- `sshpass` and `scp` for deployment

The Docker image only needs to be rebuilt when dependencies change. Source code is mounted at runtime, so edits on the host are reflected immediately.

## Smart RTU Tool

A PyQt5 desktop application (`tools/smart_rtu_tool/`) for configuring the board remotely:

- **Device Config** — Wi-Fi, Modbus RTU/TCP settings, register map sizing
- **MQTT Settings** — AWS IoT Core broker, X.509 certificate paths (browse local files, auto-uploaded to board via SSH/Serial)
- **Data Transmission** — Import registers from Excel, validate, push config to board
- **Status** — Live log, delivery status

Supports three delivery modes: MQTT (field/NAT), SSH (LAN), Serial/USB (no network).

```bash
cd tools/smart_rtu_tool
pip install -r requirements.txt
python run.py
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `docker build` fails with 404 on armhf packages | Check internet connection; the Dockerfile fetches from `ports.ubuntu.com` |
| `Host key verification failed` on deploy | Already handled — makefile uses `-o StrictHostKeyChecking=no` |
| `Permission denied` on deploy | Pass the password: `-e SSHPASS=<password>` |
| `libXXX.so: cannot open shared object file` on board | Run `make deploy-libs` to copy all shared libraries to the board |
| `${PWD}` volume mount is empty in Docker | Use `$(pwd -W)` in Git Bash on Windows |
| Docker commands fail with 500 error | Start Docker Desktop and wait for it to fully initialize |
