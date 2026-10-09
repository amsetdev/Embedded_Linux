# STM32MP157F-DK2 — Full Guide: Device Tree Edit for Modbus RS-485 (UART7) + M4-owned RTC (I2C5)

Board: **STM32MP157F-DK2**, image `openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10`
(kernel 6.6.129), stock `stm32mp157f-dk2` device tree.

This is the complete, start-to-finish record of what was done: getting the
real vendor device tree source without a full Yocto build, editing it to
enable UART7 (Modbus RTU / RS-485, DE on PE9) for Linux and I2C5 (external
RTC) for the Cortex-M4 core, compiling it, deploying it, the boot-config
pitfall that cost the most time, the application-side fix that had to go
with it, and finally baking the result into the starter package so future
SD cards boot into it with zero manual steps. Read top to bottom to redo
this from scratch on a different board/peripheral set.

---

## 0. What changed, in one table

| Peripheral | Pins | Owner | Purpose |
|---|---|---|---|
| `uart7` | TX=PE8, RX=PE7, **RTS=PE9**, CTS=PE10 | **A7 (Linux)** | Modbus RTU over RS-485. PE9 (hardware RTS) drives the transceiver's DE/RE pin automatically in hardware — no bit-banged GPIO. |
| `i2c5` | SCL=PA11, SDA=PA12 (Arduino connector D15/D14) | **M4 (Cortex-M4)** | External RTC module. M4 firmware talks to it directly; Linux never touches the bus. |

Nothing else on the board's default pin allocation was touched (touchscreen
on I2C1, PMIC/USB-C on I2C4, Bluetooth on USART2, console on UART4,
Ethernet, display, etc. are all unchanged).

---

## 1. Figure out what's actually on the board before touching anything

Don't guess pin assignments from documentation alone — confirm live, over
SSH, what's already in use:

```bash
ssh root@<board-ip> '
  cat /proc/device-tree/model
  cat /proc/device-tree/compatible
  ls /dev/ttySTM* /dev/i2c-*
  for n in 0 1 2; do echo "-- i2c-$n --"; cat /sys/bus/i2c/devices/i2c-$n/name; done
  i2cdetect -y <bus>
  dmesg | grep -iE "usart|uart"
  cat /sys/class/remoteproc/remoteproc0/state
'
```

This told us, before any edits:
- `uart4` = console (ST-LINK VCP), `ttySTM0`, always in use.
- `usart2` = onboard Bluetooth, always in use.
- `i2c1` (bus `i2c-0`) = touchscreen (0x38), HDMI TX (0x39), audio codec (0x4a).
- `i2c4` (bus `i2c-1`) = PMIC (0x33), USB-C controller (0x28) — never reuse,
  board power management depends on it.
- M4 core was idle/offline, no firmware loaded.

## 2. Get the real vendor device tree source (no full Yocto build needed)

ST's "Starter Package" (the flashable image folder,
`D:\stm32\FLASH-stm32mp1-...`) contains **prebuilt binaries only** — no
`.dts` source. The actual source comes from the separate **SOURCES**
package (the Yocto "Developer Package" download), which contains the
plain kernel.org tarball **plus** ST's vendor patch as a single `.patch`
file — not a git tree, so no `repo`/`bitbake` setup is needed just to read
or edit device trees.

```
D:\stm32\SOURCES-stm32mp-openstlinux-6.6-yocto-scarthgap-mpu-v26.02.18.tar.gz
  └── sources/ostl-linux/linux-stm32mp-6.6.116-stm32mp-r3-r0/
        ├── linux-6.6.116.tar.xz        (plain upstream kernel.org tarball)
        └── 0001-v6.6-stm32mp-r3.patch  (ST's patch: adds all stm32mp1 dts/dtsi
                                          files, pinctrl, regulator & hdp bindings)
```

> **Version note:** the flashed image is release `v26.06.10` (kernel
> 6.6.129); the only SOURCES package available was the slightly older
> `v26.02.18` (kernel 6.6.116). Board-level pin/peripheral definitions did
> not change between these point releases, confirmed by diffing the
> resulting dtb against the live board's behavior — but if ST ever
> publishes a matching `v26.06.10` SOURCES package, prefer that instead.

### Extracting just what's needed

You don't need to extract the whole kernel tree. `arch/arm/boot/dts/st/`
and `include/dt-bindings/` are enough:

```bash
mkdir -p tree && cd tree
tar xJf linux-6.6.116.tar.xz --wildcards \
  'linux-6.6.116/arch/arm/boot/dts/st/*' \
  'linux-6.6.116/arch/arm/boot/dts/Makefile' \
  'linux-6.6.116/include/dt-bindings/*'
mv linux-6.6.116/* . && rmdir linux-6.6.116

patch -p1 -N --fuzz=3 < 0001-v6.6-stm32mp-r3.patch
```

**Pitfall hit here:** most `stm32mp1*.dts*` files are pure new-file
additions by the patch, but a few (`stm32mp151.dtsi`, `stm32-pinfunc.h`,
etc.) are *modifications* of files that already exist upstream. If you
extract dt-bindings with a blind `cp -r` after the patch has already run,
you can **overwrite already-patched files with vanilla upstream ones**
(this happened — `RSVD`/pinmux macros silently disappeared and `dtc` threw
cryptic syntax errors pointing at unrelated lines). Fix: extract vanilla
files with `cp -rn` / `--update=none` (never clobber), or simply re-run the
patch a second time afterward (`patch -N` skips hunks already applied and
just re-applies anything that got reverted by the overwrite).

## 3. Understand the A7/M4 split mechanism in the device tree

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

Pin ownership follows the same idea via the pinmux "function" value:
normal peripheral pins use `AFn` (alternate function number); pins
reserved for M4 use the special `RSVD` function (`0x12` in
`stm32-pinfunc.h`), which tells Linux's pinctrl driver to configure the pin
but never let a Linux consumer request it. ST's own
`stm32mp157f-dk2-m4-examples.dts` overlay demonstrates this exact pattern
(`m4_i2c5`, `m4_uart7`, `RSVD` pinmux) — this change follows the same
convention, just with I2C5 and UART7 assigned to opposite cores from ST's
example.

## 4. Pick the actual pins

Cross-referenced `stm32mp15xx-dkx.dtsi` (board defaults) and
`stm32mp15-pinctrl.dtsi` (pin groups) against what was already confirmed
in use on the live board (§1):

- `usart3` and `uart7` were the only two UARTs left disabled/spare on the
  stock board dts.
- `uart7` was picked because its **`uart7_pins_a`** pin group (as opposed
  to the board-default `uart7_pins_c`, which only muxes TX/RX) also mux
  PE9/PE10 as real hardware RTS/CTS (`AF7`), and neither pin is used
  anywhere else on this board.
- **PE9 (RTS) → DE**, not PE10 (CTS): CTS is an *input* to the USART (used
  for hardware flow control from a peer), so it cannot drive an external
  transceiver's enable pin. RTS is an *output* the USART peripheral itself
  toggles around each transmission once RS-485 mode
  (`linux,rs485-enabled-at-boot-time`) is enabled — no GPIO bit-banging,
  no software timing jitter.
- `i2c5` was already defined in the board dtsi as a spare, disabled bus on
  PA11/PA12, confirmed physically wired to the Arduino connector's
  D15/D14 pins.

```
uart7_pins_a: uart7-0 {
    pins1 { pinmux = <STM32_PINMUX('E', 8, AF7)>; };  /* TX */
    pins2 {
        pinmux = <STM32_PINMUX('E', 7, AF7)>,          /* RX  */
                 <STM32_PINMUX('E', 10, AF7)>,          /* CTS */
                 <STM32_PINMUX('E', 9, AF7)>;           /* RTS = DE */
    };
};

m4_i2c5_pins_a: m4-i2c5-0 {
    pins {
        pinmux = <STM32_PINMUX('A', 11, RSVD)>,        /* SCL */
                 <STM32_PINMUX('A', 12, RSVD)>;          /* SDA */
    };
};
```

## 5. Write the board-variant `.dts`

[`stm32mp157f-dk2-modbus.dts`](./stm32mp157f-dk2-modbus.dts) — a thin
overlay on top of the stock `stm32mp157f-dk2.dts`, touching only three
nodes:

```dts
/dts-v1/;
#include "stm32mp157f-dk2.dts"

/ {
	model = "STMicroelectronics STM32MP157F-DK2 Modbus + M4 RTC board";
	compatible = "st,stm32mp157f-dk2-modbus", "st,stm32mp157f-dk2", "st,stm32mp157";
};

&m4_i2c5 {
	pinctrl-names = "default";
	pinctrl-0 = <&m4_i2c5_pins_a>;
	status = "okay";
};

&uart7 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart7_pins_a>;
	/delete-property/ pinctrl-1;   /* drop stale sleep/idle groups that only cover TX/RX */
	/delete-property/ pinctrl-2;
	linux,rs485-enabled-at-boot-time;
	status = "okay";
};
```

(The A7-side `&i2c5` is left untouched — it already defaults to
`status = "disabled"` in the board dtsi, which is exactly what we want.)

If your transceiver's DE pin is **active-low** instead of active-high, add
`rs485-rts-active-low;` next to `linux,rs485-enabled-at-boot-time;`.

## 6. Compile it — no root, no full SDK needed

`dtc` (device tree compiler) and `cpp` (plain C preprocessor, architecture
doesn't matter — it's just text substitution) are all that's required.

**No root/sudo for `dtc`?** Download the `.deb` and extract it locally:
```bash
apt-get download device-tree-compiler
dpkg-deb -x device-tree-compiler_*.deb ./dtc_local
./dtc_local/usr/bin/dtc --version
```

Then preprocess + compile:
```bash
cpp -nostdinc -undef -x assembler-with-cpp \
  -I tree/arch/arm/boot/dts/st -I tree/arch/arm/boot/dts -I tree/include \
  tree/arch/arm/boot/dts/st/stm32mp157f-dk2-modbus.dts \
  /tmp/out.pre.dts

dtc -I dts -O dtb -Wno-unit_address_vs_reg -Wno-avoid_default_addr_size \
  -o stm32mp157f-dk2-modbus.dtb /tmp/out.pre.dts
```

All of this is scripted in [`build_dtb.sh`](./build_dtb.sh) — tested,
reproduces the exact same dtb byte-for-byte:
```bash
./build_dtb.sh stm32mp157f-dk2-modbus.dts
```

**Sanity-check the compiled result** by decompiling it back and grepping
for the exact pinmux values, before ever touching the board:
```bash
dtc -I dtb -O dts stm32mp157f-dk2-modbus.dtb | grep -A15 'serial@40018000'
```
(`0x4808`=PE8/AF7 TX, `0x4708`=PE7/AF7 RX, `0x4a08`=PE10/AF7 CTS,
`0x4908`=PE9/AF7 RTS — confirms the pinmux macro math is right before you
waste a reboot cycle on a typo.)

## 7. Deploying — and the pitfall that actually mattered

The board boots via U-Boot's **extlinux** generic distro boot. The
tempting assumption is that `/boot/mmc1_extlinux/extlinux.conf` (a plain,
generic file with one `OpenSTLinux` label) is what controls this. **It is
not**, and editing it wastes reboot cycles for nothing.

The real boot script (`/boot/boot.scr.uimg`) does this, in order:
```
if test -e ${devtype} ${devnum}:${distro_bootpart} extlinux/${board_name}_extlinux.conf; then
    boot_syslinux_conf = extlinux/${board_name}_extlinux.conf     # <-- this one wins
else
    boot_syslinux_conf = extlinux/extlinux.conf                  # generic fallback
fi
```
`${board_name}` resolves to `stm32mp157f-dk2` on this board, so the file
that's **actually read** is:
```
/boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf
```
— note **`mmc0`**, not `mmc1` (U-Boot's mmc device numbering doesn't
necessarily match Linux's `mmcblk1` naming), and note the **board-name
suffix**. This file already had its own `DEFAULT OpenSTLinux` line and,
critically, a **different `PARTUUID`** than the generic file. Blindly
copying a label from one extlinux file to the other would have produced a
board that can't find its root filesystem.

**How to find the right file on any board:**
```bash
ssh root@<board-ip> 'strings /boot/boot.scr.uimg | grep -iE "extlinux|board_name"'
ssh root@<board-ip> 'find /boot -iname "*extlinux.conf*"'
```
Look for the `${board_name}_extlinux.conf` pattern in the script output,
then match it to the actual filename under `/boot/mmc*_extlinux/`.

### The actual working deployment

```bash
scp stm32mp157f-dk2-modbus.dtb root@<board-ip>:/boot/

ssh root@<board-ip>
cp /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf \
   /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf.bak

# Add a new label AND point DEFAULT at it (the extlinux menu's TIMEOUT is
# in tenths of a second -- "TIMEOUT 20" is 2 seconds, far too short to
# react to over a serial terminal, so relying on interactive selection
# is not realistic; just set DEFAULT).
cat >> /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf <<'EOF'
LABEL stm32mp157f-dk2-modbus
	KERNEL /uImage
	FDT /stm32mp157f-dk2-modbus.dtb
	INITRD /st-image-resize-initrd
	APPEND root=PARTUUID=<same-PARTUUID-as-the-OpenSTLinux-label-in-this-file> rootwait rw   console=${console},${baudrate}
EOF
sed -i 's/^DEFAULT .*/DEFAULT stm32mp157f-dk2-modbus/' /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf

reboot
```

### Verify after reboot

```bash
ssh root@<board-ip> '
  cat /proc/device-tree/model      # -> "...Modbus + M4 RTC board"
  ls -la /dev/ttySTM*              # ttySTM2 should now exist (uart7)
  dmesg | grep 40018000            # uart7 register base
  i2cdetect -l                     # i2c5 (0x40015000) should be ABSENT
'
```

### Rollback
```bash
mv /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf.bak \
   /boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf
rm /boot/stm32mp157f-dk2-modbus.dtb
reboot
```

## 8. The application also had to change

The existing Modbus RTU driver (`src/fieldbus/fieldbus_rtu.c` in the
`Embedded_Linux` app project) was written assuming DE was a **bit-banged
GPIO on PE10** (`libmodbus`'s `modbus_rtu_set_custom_rts()`), and it
actively **disabled** kernel RS-485 mode via a raw `TIOCSRS485` ioctl to
make room for that. Since the device tree now does hardware RS-485 on PE9
instead, that entire GPIO/callback path had to be removed:

- Deleted: `gpio_de_init/set/close`, `rtu_rts_callback`, the
  `modbus_rtu_set_custom_rts()`/`modbus_rtu_set_rts_delay()` calls, and the
  block that force-disabled kernel RS-485 (`rs485conf.flags = 0;` +
  `ioctl(fd, TIOCSRS485, ...)`).
- Kept: `modbus_rtu_set_serial_mode(ctx->mb_ctx, MODBUS_RTU_RS485)` — this
  is now the *only* RS-485-related call needed; it just confirms what the
  device tree already enabled at boot.
- `src/fieldbus/modbus.c` has the same old PE10/GPIO design, but it's
  **not in the Makefile's `SRCS`** — dead code, not part of the actual
  binary, left alone but worth removing eventually.

**Lesson:** a device tree change that moves DE from a manual GPIO to
hardware RTS is not "transparent" to existing code — any driver written
around manual DE control needs its RTS/GPIO logic *removed*, not just
repointed at a different pin.

## 9. Baking it into the starter package (skip all of the above on future SD cards)

The default `OpenSTLinux` label doesn't use a hardcoded `FDT` filename —
it uses `FDTDIR /` and lets U-Boot resolve `${fdtfile}` (auto-set from
board detection) to `stm32mp157f-dk2.dtb`. So instead of adding a new
label for every new SD card, just replace the *content* of that filename
before flashing.

**Gotcha:** this file is not a loose file in the Starter Package folder —
it's packed inside `images/stm32mp1/st-image-weston-openstlinux-weston-stm32mp1.splitted-bootfs.ext4`,
a full ext4 filesystem image that becomes the `/boot` partition on flash.
Editing it means patching a filesystem image, not just copying a file.
Loop-mounting needs root; `debugfs -w` does not:

```bash
FILE=".../images/stm32mp1/st-image-weston-openstlinux-weston-stm32mp1.splitted-bootfs.ext4"

# Always back up the image first -- this edits it in place.
cp "$FILE" "$FILE.orig-bak"

debugfs -w -R "rm /stm32mp157f-dk2.dtb" "$FILE"
debugfs -w -R "write stm32mp157f-dk2-modbus.dtb /stm32mp157f-dk2.dtb" "$FILE"
debugfs -w -R "sif /stm32mp157f-dk2.dtb mode 0100644" "$FILE"   # debugfs 'write' leaves mode 0777 -- fix it

e2fsck -fy "$FILE"   # must come back clean
```

After this, **any SD card flashed from this exact extracted folder** (via
STM32CubeProgrammer, using its existing flashlayout) boots directly into
the Modbus/M4 configuration — no extlinux editing, no menu, no post-flash
SSH steps.

Caveats:
- This only affects *this specific extracted folder*. A fresh
  re-download/re-extraction of the starter package needs this step
  redone (or just treat this folder as the permanent "golden" copy).
- To go back to stock, restore from `*.orig-bak`.
- This bakes in the device tree only — the app binary, config JSON, and
  TLS certs under `/home/root/edb_c/linking/` are not part of this image
  and still need to be copied onto any newly-flashed card separately.
- M4 firmware for the RTC is a separate remoteproc load, per board,
  regardless of SD card — the device tree change only reserves I2C5 for
  M4, it doesn't run anything there.

## 10. Reusing this for a different pin/peripheral set

1. Repeat §1 on the actual target board to confirm what's really free.
2. Repeat §4 by grepping `stm32mp15-pinctrl.dtsi` /
   `stm32mp15-m4-srm-pinctrl.dtsi` for the peripheral and pin group you
   want (search by pin letter+number in comments, e.g. `'E', 9,`).
3. Copy `stm32mp157f-dk2-modbus.dts` as a template, change the
   `&peripheral { ... }` fragments.
4. `./build_dtb.sh your-new-file.dts`, decompile-and-grep to confirm pinmux
   before deploying (§6).
5. Deploy via §7 — **always check `boot.scr.uimg` strings first** to find
   the real board-specific extlinux file for that specific board/image,
   don't assume it's the same path as last time.
6. If the peripheral you're changing has existing application code built
   around the old pin assignment (like the RS-485 DE GPIO here), budget
   time for an app-side fix too (§8) — the device tree change alone is not
   sufficient.
7. Once verified, bake into the starter package per §9 if you'll be
   reflashing multiple SD cards.
