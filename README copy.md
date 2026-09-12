# STM32MP157F-DK2 — Device Tree: Modbus RS-485 (UART7) + M4-owned RTC (I2C5)

Board: **STM32MP157F-DK2**, image `openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10`
(kernel 6.6.129), stock `stm32mp157f-dk2` device tree.

This document explains the pin/peripheral split that was implemented, why,
and exactly how to reproduce, rebuild, and deploy the change — so anyone on
the team can edit this device tree without re-deriving all of this from
scratch.

## 1. What changed

| Peripheral | Pins | Owner | Purpose |
|---|---|---|---|
| `uart7` | TX=PE8, RX=PE7, **RTS=PE9**, CTS=PE10 | **A7 (Linux)** | Modbus RTU over RS-485. PE9 (hardware RTS) drives the transceiver's DE/RE pin automatically in hardware. |
| `i2c5` | SCL=PA11, SDA=PA12 (Arduino connector D15/D14) | **M4 (Cortex-M4)** | External RTC module. M4 firmware talks to it directly; Linux never touches the bus. |

Nothing else on the board's default pin allocation was touched (touchscreen
on I2C1, PMIC/USB-C on I2C4, Bluetooth on USART2, console on UART4, Ethernet,
display, etc. are all unchanged).

### Why these pins specifically

Found by inspecting the actual vendor board `.dtsi`
(`stm32mp15xx-dkx.dtsi`) and the pin controller definitions
(`stm32mp15-pinctrl.dtsi`, `stm32mp15-m4-srm-pinctrl.dtsi`) — see §4 for how
to get these files yourself. Confirmed live against the board over SSH
(pinctrl debugfs, `i2cdetect`, `dmesg`, `/proc/device-tree`).

- `usart3` and `uart7` were the only two UARTs left disabled/spare on the
  stock board dts. `uart7` was picked because its **`uart7_pins_a`** pin
  group (as opposed to the board-default `uart7_pins_c`, which only muxes
  TX/RX) also mux PE9/PE10 as real hardware RTS/CTS (`AF7`), and neither pin
  is used anywhere else on this board.
- **PE9 (RTS) → DE**, not PE10 (CTS): CTS is an *input* to the USART (used
  for hardware flow control from a peer), so it cannot drive an external
  transceiver's enable pin. RTS is an *output* the USART peripheral itself
  toggles around each transmission when RS-485 mode
  (`linux,rs485-enabled-at-boot-time`) is enabled — no GPIO bit-banging,
  no software timing jitter.
- `i2c5` was already defined in the board dtsi as a spare, disabled bus on
  PA11/PA12, which the user confirmed is physically wired to the Arduino
  connector's D15/D14 pins.
- The vendor's own `stm32mp157f-dk2-m4-examples.dts` overlay demonstrates
  exactly this A7/M4 split pattern (`m4_i2c5`, `m4_uart7`, `RSVD` pinmux) —
  this change follows that same convention, just with I2C5 and UART7
  assigned to opposite cores from ST's example.

## 2. How the A7/M4 split actually works in DT

Every shared peripheral has **two** device tree nodes at the same register
address:

- The normal Linux driver node (e.g. `&i2c5`, compatible
  `"st,stm32mp15-i2c"`) — used when **A7/Linux** owns the peripheral.
- A stub node under the M4 "system resource manager"
  (`compatible = "rproc-srm-dev"`, labelled `m4_i2c5`, `m4_uart7`, etc., in
  `stm32mp15-m4-srm.dtsi`) — used when the **M4 firmware** owns it. Linux
  does not drive the peripheral through this node; it only reserves the
  clock/IRQ/pins so Linux's power management and pinctrl subsystems leave
  them alone. The M4 firmware (built separately in STM32CubeIDE/CubeMX,
  outside of this Linux device tree) accesses the peripheral registers
  directly.

Rule: **exactly one** of the two nodes should be `status = "okay"` for a
given peripheral, never both.

- Give a peripheral to **A7**: enable `&i2c5` / `&uart7` (the normal node),
  leave `&m4_i2c5` / `&m4_uart7` disabled (their default state).
- Give a peripheral to **M4**: enable `&m4_i2c5` / `&m4_uart7`, leave the A7
  node disabled.

Pin ownership follows the same idea via the pinmux "function" value: normal
peripheral pins use `AFn` (alternate function number); pins reserved for M4
use the special `RSVD` function (defined as `0x12` in
`stm32-pinfunc.h`), which tells Linux's pinctrl driver to configure the pin
but never let a Linux consumer request it.

## 3. The actual device tree file

[`stm32mp157f-dk2-modbus.dts`](./stm32mp157f-dk2-modbus.dts) — a small
board-variant file that includes the stock `stm32mp157f-dk2.dts` and only
overrides the three nodes described above. Compiled output:
[`stm32mp157f-dk2-modbus.dtb`](./stm32mp157f-dk2-modbus.dtb) (already built
and deployed — see §5).

To add more pins/peripherals for M4 later (following the same pattern as
ST's `-m4-examples.dts`), add more `&m4_xxx { ... status = "okay"; }`
fragments to this same file, using `m4_..._pins_a` groups from
`stm32mp15-m4-srm-pinctrl.dtsi`.

## 4. Rebuilding the .dtb from source

You do **not** need a full Yocto/bitbake environment to edit device trees —
Yocto SOURCES packages ship the actual kernel source + ST's vendor patch as
a tarball + patch file, and `dtc`/`cpp` are all that's needed to turn a
`.dts` into a `.dtb`.

1. On disk we have (from the ST OpenSTLinux Developer Package downloads):
   - `D:\stm32\SOURCES-stm32mp-openstlinux-6.6-yocto-scarthgap-mpu-v26.02.18.tar.gz`
     — contains `sources/ostl-linux/linux-stm32mp-6.6.116-stm32mp-r3-r0/`
     with the plain kernel.org tarball (`linux-6.6.116.tar.xz`) and ST's
     patch (`0001-v6.6-stm32mp-r3.patch`) that adds all STM32MP1 board
     `.dts`/`.dtsi` files and pin/regulator/hdp bindings on top of it.

   > Note: the flashed image is release `v26.06.10` (kernel 6.6.129); the
   > only SOURCES package available was the slightly older `v26.02.18`
   > (kernel 6.6.116). Pin/peripheral definitions for this board did not
   > change between these point releases, so this is safe — but if ST
   > publishes a matching `v26.06.10` SOURCES package later, prefer that
   > one and re-verify.

2. Edit or add your `.dts` file (based on `stm32mp157f-dk2-modbus.dts`).

3. Run:
   ```bash
   ./build_dtb.sh your-file.dts
   ```
   This extracts just what's needed (`arch/arm/boot/dts/st/`,
   `include/dt-bindings/`), applies ST's patch, copies your `.dts` in, and
   runs `cpp` + `dtc` to produce `your-file.dtb`. Takes well under a minute.

### No root? Get `dtc` anyway

If `device-tree-compiler` isn't installed and you don't have `sudo`, you can
still get the `dtc` binary without root:
```bash
apt-get download device-tree-compiler
dpkg-deb -x device-tree-compiler_*.deb ./dtc_local
./dtc_local/usr/bin/dtc --version
```
Then either put `./dtc_local/usr/bin` on your `PATH`, or edit
`build_dtb.sh` to call that path directly instead of the bare `dtc`.

## 5. Deploying to the board

The board boots via U-Boot's **extlinux** generic distro boot
(`/boot/mmc1_extlinux/extlinux.conf` on the target, `FDTDIR /` — it loads
whatever `.dtb` matches the boot label). `fw_printenv`/`fw_setenv` are
**not configured** on this image (`/etc/fw_env.config` missing), so instead
of touching the default boot's `fdtfile`, a new **selectable** boot entry
was added — the existing default boot is untouched and still works exactly
as before.

What was done on the target (root@192.168.1.8):
```bash
# 1. Copy the new dtb next to the existing ones
scp stm32mp157f-dk2-modbus.dtb root@<board-ip>:/boot/

# 2. Back up the boot config, then add a new menu entry
ssh root@<board-ip>
cp /boot/mmc1_extlinux/extlinux.conf /boot/mmc1_extlinux/extlinux.conf.bak
cat >> /boot/mmc1_extlinux/extlinux.conf <<'EOF'
LABEL stm32mp157f-dk2-modbus
	KERNEL /uImage
	FDT /stm32mp157f-dk2-modbus.dtb
	INITRD /st-image-resize-initrd
	APPEND root=PARTUUID=491f6117-415d-4f53-88c9-6e0de54deac6 rootwait rw   console=${console},${baudrate}
EOF
```
(The `PARTUUID` must match the root partition's actual UUID — copy it from
the existing `OpenSTLinux` label in the same file, don't hardcode the value
above blindly if you're doing this on a different board/SD card.)

### To actually boot into it

The extlinux menu (`TIMEOUT 20`, i.e. 20 × 1/10s = 2s... actually 20 in
tenths of a second = 2 seconds — check your `boot.scr` if it feels too
fast) only shows/waits when there's more than one label, which is now the
case. Connect a serial terminal to the **ST-LINK virtual COM port**
(115200 8N1), reboot the board, and either:
- let it auto-boot the default `OpenSTLinux` label (unchanged behavior), or
- interrupt and select `stm32mp157f-dk2-modbus` from the menu to boot with
  the new dtb.

### Rollback

Nothing destructive was done. To fully remove the new option:
```bash
mv /boot/mmc1_extlinux/extlinux.conf.bak /boot/mmc1_extlinux/extlinux.conf
rm /boot/stm32mp157f-dk2-modbus.dtb
```

## 6. Verifying after booting the new dtb

```bash
# Confirm the new dtb is active
cat /proc/device-tree/model
# -> "STMicroelectronics STM32MP157F-DK2 Modbus + M4 RTC board"

# UART7 should now exist and be enabled
ls -la /dev/ttySTM*          # a new ttySTM device should appear for uart7
dmesg | grep 40018000        # uart7's register base

# I2C5 should NOT show up as a Linux i2c bus anymore (it's M4-owned)
ls /dev/i2c-*
i2cdetect -l                 # i2c5 (0x40015000) should be absent
```

### Modbus (userspace, on the new UART7 tty)
Configure the port however your Modbus library expects (baud/parity/stop
bits are set by the application via `termios`, not the device tree), e.g.
with `mbpoll` or `libmodbus`:
```bash
stty -F /dev/ttySTM<N> 9600 cs8 -parenb -cstopb
# then run your Modbus RTU master/slave against /dev/ttySTM<N>
```
RS-485 direction switching (DE via PE9) is handled entirely by the kernel's
RS-485 support in the STM32 USART driver — no manual GPIO toggling needed
in the application. If your transceiver's DE pin is **active-low** instead
of active-high, add `rs485-rts-active-low;` next to
`linux,rs485-enabled-at-boot-time;` in the `&uart7` node and rebuild.

### M4 RTC
Enabling `&m4_i2c5` in Linux only reserves the bus/pins for the M4 core —
it does **not** make the RTC show up as a Linux `rtc` device, and does not
by itself start the M4. You still need to:
1. Write M4 firmware (STM32CubeIDE/CubeMX, HAL I2C driver) that talks to
   the RTC chip on I2C5 (PA11/PA12).
2. Load it via remoteproc:
   ```bash
   echo <your-firmware.elf> > /sys/class/remoteproc/remoteproc0/firmware
   echo start > /sys/class/remoteproc/remoteproc0/state
   ```
This is outside the scope of the device tree change itself.

## 7. Board facts used while working on this (for reference)

Gathered live over SSH (`root@192.168.1.8`, password-less root):
- `uart4` = console (ST-LINK VCP), `ttySTM0`, always in use.
- `usart2` = onboard Bluetooth, always in use.
- `i2c1` (bus `i2c-0`) = touchscreen (0x38), HDMI TX (0x39), audio codec
  (0x4a).
- `i2c4` (bus `i2c-1`) = PMIC (0x33), USB-C controller (0x28) — never
  reuse, board power management depends on it.
- M4 core was idle/offline, no firmware loaded, before this change.
