# ============================================================
#  STM32MP1 Cross-Compilation Makefile
#  Target : arm-linux-gnueabihf
#  Host   : WSL (Ubuntu)
# ============================================================

# --- Toolchain ---
CC      := arm-linux-gnueabihf-gcc

# --- Directories ---
SRC_DIR   := src
INC_DIR   := inc
BUILD_DIR := build

# --- Project ---
TARGET  := $(BUILD_DIR)/main

SRCS    := $(SRC_DIR)/main.c       \
           $(SRC_DIR)/settings.c     \
           $(SRC_DIR)/display.c    \
           $(SRC_DIR)/storage.c      \
           $(SRC_DIR)/modbus.c \
           $(SRC_DIR)/mqtt.c \
           $(SRC_DIR)/data.c \
           $(SRC_DIR)/mb_tcp.c \
           $(SRC_DIR)/drive_logger.c \
            $(SRC_DIR)/connection.c

OBJS    := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# --- Flags ---
CFLAGS  := -O2 \
            -I $(INC_DIR) \
            -I include

LDFLAGS := -L/usr/lib/arm-linux-gnueabihf \
            -lmodbus    \
            -lmosquitto \
            -lsqlite3   \
            -lpthread   \
            -lssl       \
            -lcrypto    \
            -lcurl      \
            -ldl        \
            -lz         \
            -lm

# --- Board Deploy ---
BOARD_USER := root
BOARD_IP   := 192.168.0.101
BOARD_DIR  := /home/root/edb_c/linking/

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean deploy flash

## Build → build/main
all: $(BUILD_DIR) $(TARGET)
	@echo " Build complete → $(TARGET)"

## Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

## Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

## Compile each src/*.c → build/*.o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo " Cleaned"

## Copy binary to board
deploy:
	scp $(TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)
	@echo " Deployed to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

## Build + deploy in one shot
flash: clean all deploy