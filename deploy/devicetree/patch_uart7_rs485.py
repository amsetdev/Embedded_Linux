#!/usr/bin/env python3
"""Turn a board's own device tree into one with UART7 in RS-485 mode, DE on PE9.

    dtc -I dtb -O dts -o board.dts stm32mp157c-dk2.dtb       # the DTB the board boots today
    python3 patch_uart7_rs485.py board.dts board-rs485.dts
    dtc -I dts -O dtb -o stm32mp157c-dk2-rs485.dtb board-rs485.dts

Only the Linux UART7 node (serial@40018000, compatible "st,stm32h7-uart") changes:
  - pinctrl-0 -> the existing "uart7-0" group: TX PE8, RX PE7, CTS PE10, RTS PE9 (AF7),
    so the USART's hardware RTS drives the transceiver's DE (and /RE);
  - pinctrl-1/-2 (sleep/idle groups that only cover TX/RX) removed, names = "default";
  - linux,rs485-enabled-at-boot-time added (--active-low also adds rs485-rts-active-low).
Everything else stays exactly as the board boots it. Refuses if the node or group is
missing, if the group isn't PE8/PE7/PE10/PE9 AF7, or if the M4 claims UART7.
See DOCS/DEVICETREE_RS485.md.
"""

import argparse
import re
import sys

EXPECTED_PINS = {0x4808, 0x4708, 0x4A08, 0x4908}   # PE8 TX, PE7 RX, PE10 CTS, PE9 RTS, all AF7


def find_node(text, header_regex):
    """(start, end) of the first node whose header matches, end = index after its '};'."""
    for m in re.finditer(header_regex, text):
        depth, i = 0, text.index("{", m.start())
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return m.start(), text.index(";", i) + 1
            i += 1
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--active-low", action="store_true", help="transceiver DE is active-low")
    args = ap.parse_args()
    text = open(args.src).read()

    # 1. the uart7-0 pin group and its phandle
    grp = find_node(text, r"(?m)^\s*uart7-0 \{")
    if not grp:
        sys.exit("no 'uart7-0' pin group in this device tree")
    body = text[grp[0]:grp[1]]
    phandle = re.search(r"phandle = <(0x[0-9a-f]+)>;", body).group(1)
    pins = {int(v, 16) for v in re.findall(r"0x[0-9a-f]+", " ".join(re.findall(r"pinmux = <([^>]*)>", body)))}
    if pins != EXPECTED_PINS:
        sys.exit(f"uart7-0 pins are {sorted(hex(p) for p in pins)}, expected PE8/PE7/PE10/PE9 AF7")

    # 2. the M4 must not own UART7
    for m in re.finditer(r"(?ms)serial@40018000 \{[^{}]*?compatible = \"rproc-srm-dev\";[^{}]*?\};", text):
        if 'status = "okay"' in m.group(0):
            sys.exit("the M4 (rproc-srm-dev) claims UART7 - not patching")

    # 3. the Linux UART7 node
    node = None
    for m in re.finditer(r"(?m)^\s*serial@40018000 \{", text):
        span = find_node(text[m.start():], r"serial@40018000 \{")
        candidate = (m.start(), m.start() + span[1])
        if 'compatible = "st,stm32h7-uart";' in text[candidate[0]:candidate[1]]:
            node = candidate
            break
    if not node:
        sys.exit("no Linux UART7 node (serial@40018000, st,stm32h7-uart)")
    n = text[node[0]:node[1]]
    if "rs485-enabled-at-boot-time" in n:
        sys.exit("UART7 already has RS-485 enabled - nothing to do")
    indent = re.search(r"\n(\s*)status = ", n).group(1)
    n = re.sub(r"\n\s*pinctrl-[12] = <[^>]*>;", "", n)
    n = re.sub(r'pinctrl-names = "[^"]*";', 'pinctrl-names = "default";', n)
    n = re.sub(r"pinctrl-0 = <[^>]*>;", f"pinctrl-0 = <{phandle}>;", n)
    extra = f"\n{indent}linux,rs485-enabled-at-boot-time;"
    if args.active_low:
        extra += f"\n{indent}rs485-rts-active-low;"
    n = re.sub(r'(\n\s*status = "okay";)', r"\1" + extra.replace("\\", "\\\\"), n, count=1)
    open(args.dst, "w").write(text[:node[0]] + n + text[node[1]:])
    print(f"UART7: pinctrl-0 -> uart7-0 ({phandle}), sleep/idle groups removed, RS-485 enabled"
          + (" (RTS active-low)" if args.active_low else ""))


if __name__ == "__main__":
    main()
