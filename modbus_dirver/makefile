# ── EDIT THESE ──────────────────────────────────────────────────────────────
KDIR        ?= /kernel
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