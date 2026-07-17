# UART7 Enable on STM32MP157F-DK2 (A7 Linux Kernel)
## Complete Step-by-Step Guide

---

## Overview

```
Goal: Enable UART7 as /dev/ttySTM2 on A7 Linux kernel
      so any app (libmodbus, minicom, pyserial) can use it directly.

Hardware:
  UART7 TX  → PA8  → Board CN2 Arduino pin A0
  UART7 RX  → PA7  → Board CN2 Arduino pin A1
  RS485 DE  → PE10 → Board (gpiochip4 line 10)

Result:
  /dev/ttySTM2 = UART7 real kernel tty device
  PE10 = manually controlled GPIO for RS485 direction
```

---

## Prerequisites

- STM32MP157F-DK2 board
- WSL (Windows Subsystem for Linux) or Linux PC
- SSH access to board
- Board IP: change `192.168.0.101` to your board IP everywhere below

---

## PART 1 — WSL Setup

### Step 1 — Install device tree compiler on WSL

```bash
sudo apt-get update
sudo apt-get install device-tree-compiler
```

### Step 2 — Copy DTB from board to WSL

```bash
scp root@192.168.0.101:/boot/stm32mp157f-dk2.dtb .
```

### Step 3 — Decompile DTB to editable DTS

```bash
dtc -I dtb -O dts -o dk2.dts stm32mp157f-dk2.dtb
```

Warnings are normal — ignore them.

### Step 4 — Enable UART7 in DTS (change status disabled → okay)

```bash
python3 << 'EOF'
with open('dk2.dts', 'r') as f:
    content = f.read()

old = 'serial@40018000 {\n\t\t\tcompatible = "st,stm32h7-uart";\n\t\t\treg = <0x40018000 0x400>;\n\t\t\tinterrupts-extended = <0x18 0x20 0x04>;\n\t\t\tclocks = <0x0c 0x9a>;\n\t\t\twakeup-source;\n\t\t\tpower-domains = <0x19>;\n\t\t\tstatus = "disabled";'

new = 'serial@40018000 {\n\t\t\tcompatible = "st,stm32h7-uart";\n\t\t\treg = <0x40018000 0x400>;\n\t\t\tinterrupts-extended = <0x18 0x20 0x04>;\n\t\t\tclocks = <0x0c 0x9a>;\n\t\t\twakeup-source;\n\t\t\tpower-domains = <0x19>;\n\t\t\tstatus = "okay";'

if old in content:
    content = content.replace(old, new, 1)
    with open('dk2.dts', 'w') as f:
        f.write(content)
    print("SUCCESS: uart7 status = okay")
else:
    print("ERROR: pattern not found")
    idx = content.find('serial@40018000')
    print(repr(content[idx:idx+300]))
EOF
```

### Step 5 — Verify the change

```bash
grep -A10 "serial@40018000" dk2.dts | head -12
# Must show: status = "okay";
```

### Step 6 — Recompile DTS back to DTB

```bash
dtc -I dts -O dtb -o stm32mp157f-dk2-uart7.dtb dk2.dts 2>/dev/null
ls -la stm32mp157f-dk2-uart7.dtb
# Must exist and be ~116KB
```

### Step 7 — Deploy new DTB to board

```bash
# Backup original first
ssh root@192.168.0.101 "cp /boot/stm32mp157f-dk2.dtb /boot/stm32mp157f-dk2.dtb.bak"

# Copy new DTB — overwrite original so FDTDIR/ loads it automatically
scp stm32mp157f-dk2-uart7.dtb root@192.168.0.101:/boot/stm32mp157f-dk2.dtb
```

---

## PART 2 — Board Setup

### Step 8 — Reboot board

```bash
ssh root@192.168.0.101 "reboot"
```

### Step 9 — Verify UART7 is enabled after reboot

```bash
ssh root@192.168.0.101

# Check device tree loaded correctly
cat /proc/device-tree/soc/serial@40018000/status
# Must print: okay

# Check kernel registered it
dmesg | grep "40018000"
# Must show: 40018000.serial: ttySTM2 at MMIO 0x40018000
```

### Step 10 — Create device node (needed every boot unless udev rule added)

```bash
# Check what ttySTM number was assigned
dmesg | grep "40018000"
# Look for: ttySTM2 (or ttySTM1, ttySTM3 — use whatever it shows)

# Create node (204=major, 66=minor for ttySTM2)
mknod /dev/ttySTM2 c 204 66
chmod 666 /dev/ttySTM2
ls -la /dev/ttySTM2
# Must show: crw-rw-rw- (character device, not regular file)
```

> **Note:** If you see `-rw-r--r--` (regular file) instead of `crw-`,
> someone did `echo > /dev/ttySTM2` accidentally.
> Fix: `rm /dev/ttySTM2` then redo the mknod command.

### Step 11 — Make device node permanent (survives reboot)

```bash
# Create udev rule
echo 'KERNEL=="ttySTM2", MODE="0666"' > /etc/udev/rules.d/99-ttySTM2.rules

# Create startup script
cat > /etc/init.d/uart7-init.sh << 'SCRIPT'
#!/bin/sh
if [ ! -c /dev/ttySTM2 ]; then
    mknod /dev/ttySTM2 c 204 66
fi
chmod 666 /dev/ttySTM2
stty -F /dev/ttySTM2 9600 cs8 -cstopb -parenb raw
SCRIPT

chmod +x /etc/init.d/uart7-init.sh
echo "/etc/init.d/uart7-init.sh" >> /etc/rc.local
chmod +x /etc/rc.local
```

### Step 12 — Test UART7 works

```bash
# Set baud rate
stty -F /dev/ttySTM2 9600 cs8 -cstopb -parenb raw
echo $?
# Must return: 0

# Send test data (connect TX→RX loopback wire to test)
echo "Hello UART7" > /dev/ttySTM2
# No error = TX working

# Or use minicom
minicom -D /dev/ttySTM2 -b 9600
```

---

## PART 3 — RS485 Direction Pin PE10

PE10 (gpiochip4 line 10) controls the RS485 DE/RE pin.
It is controlled manually from userspace using Linux GPIO chardev API.

### Step 13 — Verify PE10 is free

```bash
gpioset gpiochip4 10=1 && echo "PE10 FREE" || echo "PE10 BUSY"
gpioset gpiochip4 10=0
```

Must show `PE10 FREE`.

### Step 14 — Test PE10 toggles correctly

```bash
# Set HIGH (TX mode) — measure ~3.3V on PE10 with multimeter
gpioset gpiochip4 10=1
sleep 1

# Set LOW (RX mode) — measure ~0V on PE10
gpioset gpiochip4 10=0
```

### Step 15 — PE10 is controlled in app code

The `main_display.c` app uses Linux GPIO chardev ioctl:

```c
#define RS485_GPIOCHIP  "/dev/gpiochip4"
#define RS485_GPIO_LINE  10

// Before TX — PE10 HIGH
rs485_tx();   // sets PE10 = 1

// libmodbus read/write happens here

// After TX — PE10 LOW  
rs485_rx();   // sets PE10 = 0
```

No DTS change needed for PE10 — it is a free GPIO controlled entirely from userspace.

---

## PART 4 — Build and Run App

### Step 16 — Build on board

```bash
cd ~/edb_c/driver_test

gcc -O2 -o main_display main_display.c \
    -lmodbus -lmosquitto -lsqlite3 -lpthread

# Or cross-compile on WSL:
# arm-linux-gnueabihf-gcc -O2 -o main_display main_display.c \
#     -lmodbus -lmosquitto -lsqlite3 -lpthread
# scp main_display root@192.168.0.101:~/edb_c/driver_test/
```

### Step 17 — Create settings.conf

```bash
cat > ~/edb_c/driver_test/settings.conf << 'EOF'
modbus_port=/dev/ttyACM0
modbus_baud=9600
modbus_slave=1
mqtt_broker=3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud
mqtt_port=8883
mqtt_user=prasad
mqtt_pass=prasad#12$A
interval=30
EOF
```

### Step 18 — Create registers.csv (your Modbus register map)

```bash
cat > ~/edb_c/driver_test/registers.csv << 'EOF'
label,address,register_type,unit
Voltage,0,holding,V
Current,1,holding,A
Power,2,holding,W
Frequency,3,holding,Hz
Temperature,4,holding,C
Status,5,holding,
EOF
```

### Step 19 — Run

```bash
cd ~/edb_c/driver_test
./main_display
```

Expected output:
```
=== MODBUS RTU READER — STM32MP157F-DK2 ===
  Port  : /dev/ttyACM0 @ 9600  Slave: 1
  RS485 : DE=PE10 (gpiochip4 line 10) manual control
[RS485] PE10 GPIO init OK — DE pin ready
[INFO] Modbus connected — PE10 DE controlled manually
[INFO] READ DONE — ok:6 fail:0 time:1s
[INFO] MB cycle 1 done — 6/6 ok
[INFO] Published XXX bytes
```

---

## Troubleshooting

### Problem: `/dev/ttyACM0` not appearing after reboot
```bash
dmesg | grep "40018000"
# If no output → DTB not loaded correctly
# Fix: repeat Step 7 and Step 8
```

### Problem: `stty: /dev/ttyACM0: No such device or address`
```bash
# Device node exists but kernel didn't register it
# Check DTB status
cat /proc/device-tree/soc/serial@40018000/status
# If shows "disabled" → old DTB loaded, repeat Steps 7-8
```

### Problem: `mknod: /dev/ttySTM2: File exists` but it's a regular file
```bash
rm /dev/ttyACM0
mknod /dev/ttyACM0 c 204 66
chmod 666 /dev/ttyACM0
```

### Problem: `modbus_connect: Inappropriate ioctl for device`
```bash
# Wrong device — not a real tty
# Use /dev/ttySTM2 not /dev/modbus_uart
sed -i 's|modbus_port=.*|modbus_port=/dev/ttyACM0|' settings.conf
```

### Problem: PE10 stuck HIGH (3.3V always)
```bash
# Check if kernel claimed PE10 via DTS rts-gpios
ls /proc/device-tree/soc/serial@40018000/ | grep rts
# If rts-gpios shown → kernel owns it, restore uart7-only DTB:
cp /boot/stm32mp157f-dk2-uart7.dtb /boot/stm32mp157f-dk2.dtb
reboot
```

### Problem: Modbus timeout / no response
```bash
# Check PE10 toggles during TX
# Use multimeter on PE10 — should pulse HIGH during each request
# Check RS485 module wiring:
#   UART7 TX (PA8) → RS485 DI
#   UART7 RX (PA7) → RS485 RO
#   PE10           → RS485 DE and RE (tied together)
```

### Restore original DTB if anything breaks
```bash
cp /boot/stm32mp157f-dk2.dtb.bak /boot/stm32mp157f-dk2.dtb
reboot
```

---

## Hardware Wiring Summary

```
STM32MP157F-DK2          RS485 Module          Modbus Slave
─────────────────        ────────────          ────────────
CN2 Arduino A0 (PA8) ──► DI (Data In)
CN2 Arduino A1 (PA7) ◄── RO (Receiver Out)
PE10 pin         ───────► DE (Driver Enable)
PE10 pin         ───────► RE (Receiver Enable, tie to DE)
GND              ───────── GND
                           A ────────────────── A
                           B ────────────────── B
```

---

## File Summary

| File | Description |
|------|-------------|
| `dk2.dts` | Decompiled device tree source (edit on WSL) |
| `stm32mp157f-dk2-uart7.dtb` | Compiled DTB with UART7 enabled |
| `main_display.c` | Main app with PE10 manual GPIO control |
| `settings.conf` | Runtime configuration |
| `registers.csv` | Modbus register map |

---

## Quick Reference Commands

```bash
# Check UART7 status in live device tree
cat /proc/device-tree/soc/serial@40018000/status

# Check which ttySTM number UART7 got
dmesg | grep "40018000"

# Create device node
mknod /dev/ttyACM0 c 204 66 && chmod 666 /dev/ttySTM2

# Test UART7 baud rate
stty -F /dev/ttyACM0 9600

# Check PE10 state (0=RX mode, 1=TX mode)
gpioget gpiochip4 10

# Restore original DTB
cp /boot/stm32mp157f-dk2.dtb.bak /boot/stm32mp157f-dk2.dtb && reboot
```
