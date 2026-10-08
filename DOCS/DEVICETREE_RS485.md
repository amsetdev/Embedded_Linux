# UART7 RS-485 (DE on PE9) — device tree for the CI DK2 and other 157C-DK2 boards

`README_Devicetree.md` describes the change for an STM32MP**157F**-DK2 with a hand-written
`.dts` (not in this repo). The CI board `192.168.1.26` is an **STM32MP157C-DK2**
(`/proc/device-tree/model`), and its image never had the change. This page is the
reproducible way for 157C boards: patch **the board's own DTB**, so nothing but UART7 changes.

## 1. What was wrong (2026-10-08, CI DK2)

| Check (on the board) | Found |
|---|---|
| `cat /proc/device-tree/model` | `STMicroelectronics STM32MP157C-DK2 Discovery Board` |
| boot config read by U-Boot | `/boot/mmc0_extlinux/stm32mp157c-dk2_extlinux.conf`, `DEFAULT OpenSTLinux`, `FDTDIR /` → `/boot/stm32mp157c-dk2.dtb` |
| UART7 (`serial@40018000`) pinctrl | stock group `uart7-2`: PE8 TX, PE7 RX only |
| PE9 in `/sys/kernel/debug/pinctrl/*/pinmux-pins` | `UNCLAIMED` (not UART7_RTS) |
| `linux,rs485-enabled-at-boot-time` | missing in every DK2 DTB on `/boot` |

Effect: the transceiver's DE/RE is not driven by the USART, the board **transmits but never
receives** the slave's reply (HIL `test_03_modbus_rtu.py`: requests reach the RS485 adapter,
the simulated slave answers, the board times out).

## 2. Build the patched DTB (on any PC, no kernel sources)

```bash
scp root@<board>:/boot/stm32mp157c-dk2.dtb .                 # what the board boots today
sudo apt install device-tree-compiler                          # or: docker run … ubuntu:22.04
dtc -q -I dtb -O dts -o board.dts stm32mp157c-dk2.dtb
python3 deploy/devicetree/patch_uart7_rs485.py board.dts board-rs485.dts   # --active-low if DE is active-low
dtc -q -I dts -O dtb -o stm32mp157c-dk2-rs485.dtb board-rs485.dts

# check: only the UART7 node differs, and it uses uart7-0 (0x4808 0x4708 0x4a08 0x4908)
dtc -q -I dtb -O dts stm32mp157c-dk2.dtb       > a.dts
dtc -q -I dtb -O dts stm32mp157c-dk2-rs485.dtb > b.dts
diff a.dts b.dts
```
Expected diff (phandle numbers depend on the image):
```
<  pinctrl-names = "default\0sleep\0idle";
<  pinctrl-0 = <0x65>;  pinctrl-1 = <0x66>;  pinctrl-2 = <0x67>;
>  linux,rs485-enabled-at-boot-time;
>  pinctrl-names = "default";
>  pinctrl-0 = <0x137>;          # the uart7-0 group: PE8 TX, PE7 RX, PE10 CTS, PE9 RTS (AF7)
```
The script refuses if the group isn't exactly those pins or if the M4 claims UART7.

## 3. Install it as a new boot label (the stock DTB stays untouched)

```bash
scp stm32mp157c-dk2-rs485.dtb root@<board>:/boot/
ssh root@<board>
cd /boot/mmc0_extlinux
cp stm32mp157c-dk2_extlinux.conf stm32mp157c-dk2_extlinux.conf.bak
# copy the OpenSTLinux label (same KERNEL / INITRD / APPEND incl. its PARTUUID), FDT = the new file
awk '/^LABEL OpenSTLinux$/{f=1; print "LABEL stm32mp157c-dk2-rs485"; next}
     f && /^LABEL /{f=0} f && /FDTDIR/{print "\tFDT /stm32mp157c-dk2-rs485.dtb"; next} f{print}' \
    stm32mp157c-dk2_extlinux.conf >> stm32mp157c-dk2_extlinux.conf
sed -i 's/^DEFAULT .*/DEFAULT stm32mp157c-dk2-rs485/' stm32mp157c-dk2_extlinux.conf
grep -A6 "LABEL stm32mp157c-dk2-rs485" stm32mp157c-dk2_extlinux.conf   # review before rebooting
sync && reboot
```

## 4. Verify after the reboot

```bash
ssh root@<board> '
  grep -E "pin 73 |pin 74 " /sys/kernel/debug/pinctrl/*/pinmux-pins   # PE9, PE10: device 40018000.serial function af7
  ls /dev/ttySTM2
  systemctl is-active gateway'
```
Then retry the `hil-tests` job: `test_03_modbus_rtu.py` should pass.

## 5. Roll back

```bash
cd /boot/mmc0_extlinux && mv stm32mp157c-dk2_extlinux.conf.bak stm32mp157c-dk2_extlinux.conf
rm /boot/stm32mp157c-dk2-rs485.dtb && sync && reboot
```
If the board doesn't come up at all: put its SD card in a PC, mount the `bootfs` partition
(`mmcblk1p8` on the board) and do the same two file operations there.

## 6. History

| Date | Board | What |
|---|---|---|
| 2026-10-08 | CI DK2 `192.168.1.26` (157C-DK2) | `stm32mp157c-dk2-rs485.dtb` (sha256 `7a14c0a7…a4e510`, built from the board's own `stm32mp157c-dk2.dtb`) installed as label `stm32mp157c-dk2-rs485`, made `DEFAULT`; backup `stm32mp157c-dk2_extlinux.conf.bak`. After the reboot PE7/PE8/PE9/PE10 are `40018000.serial function af7`, kernel RS-485 `ENABLED, RTS_ON_SEND`; HIL `test_03_modbus_rtu.py` 5/5 passed |
