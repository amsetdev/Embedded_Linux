# Makefile — STM32MP157F-DK2 M4 UART7 bridge firmware
#
# Prerequisites (install in WSL):
#   sudo apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi
#
# STM32CubeMP1 path (Windows-side via /mnt/c or WSL home):
#   make CUBE=/mnt/c/Users/mauli/Desktop/linux/STM32CubeMP1
# Output: uart7_bridge.elf  (copy to /lib/firmware/ on board)

CUBE ?= $(HOME)/STM32CubeMP1

TARGET    = uart7_bridge
BUILD_DIR = build

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
LD      = arm-none-eabi-gcc

# ── Directory shortcuts ───────────────────────────────────────────────
HAL_DIR     = $(CUBE)/Drivers/STM32MP1xx_HAL_Driver
EXAMPLE_DIR = $(CUBE)/Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo
OPENAMP_DIR = $(CUBE)/Middlewares/Third_Party/OpenAMP

# ── Source files (irq.c excluded — handled separately, name conflict) ─
SRCS  = main_uart7.c
SRCS += stm32mp1xx_hal_msp.c
SRCS += platform.c

SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_cortex.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_gpio.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_rcc.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_rcc_ex.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_uart.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_uart_ex.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_ipcc.c
SRCS += $(HAL_DIR)/Src/stm32mp1xx_hal_dma.c

SRCS += $(EXAMPLE_DIR)/Src/openamp.c
SRCS += $(EXAMPLE_DIR)/Src/openamp_log.c
SRCS += $(EXAMPLE_DIR)/Src/mbox_ipcc.c
SRCS += $(EXAMPLE_DIR)/Src/rsc_table.c
SRCS += $(EXAMPLE_DIR)/Src/lock_resource.c
SRCS += $(EXAMPLE_DIR)/Src/stm32mp1xx_it.c
SRCS += $(EXAMPLE_DIR)/Src/system_stm32mp1xx.c
SRCS += $(EXAMPLE_DIR)/STM32CubeIDE/CM4/Application/Startup/startup_stm32mp157caax.s
SRCS += $(EXAMPLE_DIR)/STM32CubeIDE/CM4/Application/User/syscalls.c

SRCS += $(OPENAMP_DIR)/open-amp/lib/rpmsg/rpmsg.c
SRCS += $(OPENAMP_DIR)/open-amp/lib/rpmsg/rpmsg_virtio.c
SRCS += $(OPENAMP_DIR)/open-amp/lib/virtio/virtio.c
SRCS += $(OPENAMP_DIR)/open-amp/lib/virtio/virtqueue.c
SRCS += $(OPENAMP_DIR)/open-amp/lib/remoteproc/remoteproc_virtio.c

SRCS += $(OPENAMP_DIR)/libmetal/lib/init.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/io.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/log.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/shmem.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/device.c

SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/generic_init.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/generic_device.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/generic_io.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/generic_shmem.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/condition.c
SRCS += $(OPENAMP_DIR)/libmetal/lib/system/generic/time.c

# irq.c appears in both libmetal/lib/ and libmetal/lib/system/generic/ —
# compile with unique output names to avoid the collision.
METAL_IRQ_SRC   = $(OPENAMP_DIR)/libmetal/lib/irq.c
GENERIC_IRQ_SRC = $(OPENAMP_DIR)/libmetal/lib/system/generic/irq.c

# ── Include paths ─────────────────────────────────────────────────────
INC  = -I.
INC += -I$(HAL_DIR)/Inc
INC += -I$(CUBE)/Drivers/CMSIS/Device/ST/STM32MP1xx/Include
INC += -I$(CUBE)/Drivers/CMSIS/Include
INC += -I$(EXAMPLE_DIR)/Inc
INC += -I$(OPENAMP_DIR)/open-amp/lib/include
INC += -I$(OPENAMP_DIR)/libmetal/lib/include

# ── Compiler/linker flags ─────────────────────────────────────────────
CPU  = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

DEFS = -DSTM32MP157Fxx -DCORE_CM4 -DUSE_HAL_DRIVER
DEFS += -DMETAL_INTERNAL
DEFS += -DHAL_UART_MODULE_ENABLED -DHAL_IPCC_MODULE_ENABLED -DNDEBUG
DEFS += -D__LOG_TRACE_IO_

CFLAGS = $(CPU) $(DEFS) $(INC) -O2 -g -Wall \
         -ffunction-sections -fdata-sections -fstack-usage

LDFLAGS = $(CPU) -T stm32mp157f_dk2_m4.ld \
          -Wl,--gc-sections -Wl,-Map=$(TARGET).map \
          -nostartfiles \
          -Wl,--start-group -lc -lm -lgcc -Wl,--end-group

# ── Object list — all in local build/ (avoids /mnt/c write issues) ────
VPATH = $(sort $(dir $(SRCS)))

OBJS  = $(addprefix $(BUILD_DIR)/, $(notdir $(patsubst %.c,%.o,$(filter %.c,$(SRCS)))))
OBJS += $(addprefix $(BUILD_DIR)/, $(notdir $(patsubst %.s,%.o,$(filter %.s,$(SRCS)))))
OBJS += $(BUILD_DIR)/metal_irq.o
OBJS += $(BUILD_DIR)/generic_irq.o

# ── Build rules ───────────────────────────────────────────────────────
$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	arm-none-eabi-size $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/metal_irq.o: $(METAL_IRQ_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/generic_irq.o: $(GENERIC_IRQ_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(TARGET).elf $(TARGET).map

.PHONY: clean
