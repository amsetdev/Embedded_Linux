# STM32MP1 Industrial IoT Gateway (Cortex-A7)

Modbus RTU/TCP data acquisition and MQTT cloud publishing for STM32MP157F-DK2.

# for RTC test
m4 status/stop/add firmware/check which firmware/ start m4 >> commands : 
cat /sys/class/remoteproc/remoteproc0/state
echo stop > /sys/class/remoteproc/remoteproc0/state
echo I2C_TwoBoards_ComIT_CM4.elf > /sys/class/remoteproc/remoteproc0/firmware
cat /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state

// run this to check the RTC registers for heartbeat counter+time+date on the M4 side:
//in hex: 
 devmem2 0x38000014 w ; devmem2 0x38000024 w ; devmem2 0x38000028 w
//In decimal:
 watch -n 1 '
hb=$(devmem2 0x38000014 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
t=$(devmem2 0x38000024 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
d=$(devmem2 0x38000028 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
printf "HEARTBEAT : %d\n" "$hb"
printf "TIME      : %02d:%02d:%02d\n" "$(( (t) >> 16 & 0xFF ))" "$(( (t) >> 8 & 0xFF ))" "$(( (t) & 0xFF ))"
printf "DATE      : DOW=%d 20%02d-%02d-%02d\n" "$(( (d) >> 24 & 0xFF ))" "$(( (d) >> 16 & 0xFF ))" "$(( (d) >> 8 & 0xFF ))" "$(( (d) & 0xFF ))"
'


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

# 3. Build the project
docker run --rm -v ${PWD}:/project stm32mp1-build


The ARM binary is output at `build/main`.

## Build Commands

| Action | Command |
|--------|---------|
| Build | `docker run --rm -v ${PWD}:/project stm32mp1-build` |
| Build + Deploy | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS="Amset@123" stm32mp1-build make flash` |
| Deploy only | `docker run --rm -v ${PWD}:/project --network host -e SSHPASS=<password> stm32mp1-build make deploy` |
| Clean | `docker run --rm -v ${PWD}:/project stm32mp1-build make clean` |
| Shell | `docker run --rm -it -v ${PWD}:/project stm32mp1-build bash` |

Replace `<password>` with the board's root SSH password.

**Note:** These commands use PowerShell syntax (`${PWD}`). On Linux/WSL bash, use `$(pwd)` instead.

## Deploy Configuration

The board connection is configured in `makefile`:

```makefile
BOARD_USER := root
BOARD_IP   := 192.168.1.100
BOARD_DIR  := /home/root/edb_c/linking/
```

Update `BOARD_IP` to match your board's IP address. You can also override at runtime:

```bash
docker run --rm -v ${PWD}:/project --network host -e SSHPASS="Amset@123" stm32mp1-build make deploy BOARD_IP=<your-board-ip>
```

## Running on the Board

```bash
# SSH into the board
ssh root@192.168.1.115

# Run the binary
cd /home/root/edb_c/linking/
./main

# Run in background
./main &

# Check if running
ps aux | grep main

# Stop
killall main
```

## Project Structure

```
Embedded_Linux/
├── src/                  Application source files
│   ├── main.c            Entry point
│   ├── modbus.c          Modbus RTU communication
│   ├── mqtt.c            MQTT publish (HiveMQ, TLS)
│   ├── mb_tcp.c          Modbus TCP client
│   ├── data.c            Register data management
│   ├── storage.c         Offline store-and-forward (SD card)
│   ├── settings.c        Configuration file parser
│   ├── display.c         DRM display output
│   ├── connection.c      Network connectivity monitor
│   └── drive_logger.c    Drive log collection and upload
├── inc/                  Header files
├── modbus_dirver/        Linux kernel module (Modbus UART driver)
├── build/                Compiled output (generated)
├── Dockerfile            Cross-compilation environment
├── makefile              Build, clean, deploy, flash targets
└── .gitignore
```

## Docker Environment

The Dockerfile provides a reproducible build environment based on Ubuntu 22.04 with:

- `arm-linux-gnueabihf-gcc` cross-compiler
- ARM (`armhf`) libraries: libmodbus, libmosquitto, libsqlite3, libssl, libcurl, zlib
- `sshpass` and `scp` for deployment

The Docker image only needs to be rebuilt when dependencies change. Source code is mounted at runtime, so edits on the host are reflected immediately.

## Kernel Module (modbus_dirver)

Building the kernel module requires STM32MP1 kernel headers:

```bash
docker run --rm -v ${PWD}:/project -v /path/to/kernel-headers:/kernel stm32mp1-build make -C modbus_dirver
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `docker build` fails with 404 on armhf packages | Check internet connection; the Dockerfile fetches from `ports.ubuntu.com` |
| `Host key verification failed` on deploy | Already handled — makefile uses `-o StrictHostKeyChecking=no` |
| `Permission denied` on deploy | Pass the password: `-e SSHPASS=<password>` |
| `libcurl.so.4: no version information` on board | Safe to ignore — version mismatch between build and board libcurl |
| Docker commands fail with 500 error | Start Docker Desktop and wait for it to fully initialize |
