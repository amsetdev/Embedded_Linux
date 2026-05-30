# Makefile for modbus_uart_driver.ko
# Cross-compile on WSL for STM32MP1 A7 (ARM Cortex-A7, 32-bit)
#
# Requirements:
#   sudo apt install gcc-arm-linux-gnueabihf
#
# Set KDIR to your kernel headers — pick one option:
#
#   Option A — Yocto build output:
#     KDIR = /path/to/yocto/tmp/work/stm32mp1-.../linux-stm32mp-5.x.y/
#
#   Option B — ST OpenSTLinux SDK (recommended):
#     Install SDK from st.com, then:
#     KDIR = $(SDK_PATH)/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi/usr/src/kernel
#
#   Option C — WSL kernel (build/compile check only, not for board):
#     KDIR = $(HOME)/wsl-kernel

# ── EDIT THESE ──────────────────────────────────────────────────────────────
KDIR        ?= /path/to/kernel/headers
CROSS       ?= arm-linux-gnueabihf-
BOARD_IP    ?= 192.168.1.102
BOARD_PATH  ?= /home/root/
# ────────────────────────────────────────────────────────────────────────────

ARCH        := arm
PWD         := $(shell pwd)
obj-m       := modbus_uart_driver.o

# ── Targets ─────────────────────────────────────────────────────────────────

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) clean

deploy: all
	scp modbus_uart_driver.ko root@$(BOARD_IP):$(BOARD_PATH)
	@echo "Deployed — on board run:  insmod $(BOARD_PATH)modbus_uart_driver.ko"

info:
	@echo "KDIR  = $(KDIR)"
	@echo "CROSS = $(CROSS)"
	@echo "ARCH  = $(ARCH)"
	@modinfo modbus_uart_driver.ko 2>/dev/null || echo "(not built yet)"

.PHONY: all clean deploy info