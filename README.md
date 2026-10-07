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

The ARM binary is output at `build/main`, its runtime libraries in `build/lib/`.

## Build Commands

| Action | Command |
|--------|---------|
| Build | `docker run --rm -v ${PWD}:/project stm32mp1-build` |
| Installable package (`build/gateway-<ver>.tar.gz`) | `docker run --rm -v ${PWD}:/project stm32mp1-build make package` |
| Build + install on a board | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make install-board BOARD=root@<board-ip>` |
| Clean | `docker run --rm -v ${PWD}:/project stm32mp1-build make clean` |
| Shell | `docker run --rm -it -v ${PWD}:/project stm32mp1-build bash` |

Replace `<password>` with the board's root SSH password (or use an SSH key and drop `-e SSHPASS`).
After changing `tools/ci/install_toolchain.sh`, rebuild the image (`docker build -t stm32mp1-build .`).

**Note (Windows):** If `${PWD}` doesn't mount correctly in Git Bash, use `$(pwd -W)` instead. In PowerShell, `${PWD}` works as-is.

## On the Board

The application is installed as a systemd service by `deploy/install.sh`:
`/opt/gateway/bin/gateway`, configuration in `/etc/gateway/`, data in `/var/lib/gateway/`,
logs in journald. Layout, install, update, rollback and the migration from the old
`/home/root/edb_c/linking` setup: **[`DOCS/DEPLOYMENT.md`](DOCS/DEPLOYMENT.md)**.

```bash
systemctl status gateway          # running?
journalctl -u gateway -f          # logs
systemctl reload gateway          # re-read config + registers (SIGHUP)
/opt/gateway/bin/gateway --version
```

CI/CD, tests and the release process: [`DOCS/CI_CD_GUIDE.md`](DOCS/CI_CD_GUIDE.md),
[`DOCS/TESTS_GUIDE.md`](DOCS/TESTS_GUIDE.md).

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
├── makefile                    Build, package, install-board targets
├── deploy/                     gateway.service, install.sh (board installer)
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
| `Host key verification failed` on install | The board was re-flashed: `ssh-keygen -R <board-ip>` |
| `Permission denied` on install | Pass the password: `-e SSHPASS=<password>`, or install an SSH key |
| `libXXX.so: cannot open shared object file` on board | Reinstall the package (`/opt/gateway/lib` is missing a library); never copy libraries into `/usr/lib` |
| `${PWD}` volume mount is empty in Docker | Use `$(pwd -W)` in Git Bash on Windows |
| Docker commands fail with 500 error | Start Docker Desktop and wait for it to fully initialize |
