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
OTA_DIR   := ota
VENDOR_DIR := ota/vendor/cjson

# --- Project ---
TARGET  := $(BUILD_DIR)/main

SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/settings.c \
        $(SRC_DIR)/display.c \
        $(SRC_DIR)/storage.c \
        $(SRC_DIR)/modbus.c \
        $(SRC_DIR)/mqtt.c \
        $(SRC_DIR)/data.c \
        $(SRC_DIR)/mb_tcp.c \
        $(SRC_DIR)/drive_logger.c \
        $(SRC_DIR)/connection.c \
        $(SRC_DIR)/https.c \
        $(SRC_DIR)/rtc.c \
        $(SRC_DIR)/wifi.c \
        $(SRC_DIR)/network_manager.c \
        $(SRC_DIR)/ethernet.c \
        $(SRC_DIR)/json.c \
        $(OTA_DIR)/ota.c \
        $(OTA_DIR)/ota_config.c \
        $(OTA_DIR)/ota_network.c \
        $(OTA_DIR)/ota_install.c \
        $(VENDOR_DIR)/cJSON.c

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRCS)))

# --- Flags ---
CFLAGS := -O2 \
          -I inc \
          -I include \
          -I ota \
          -I ota/vendor

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
BOARD_IP   := 192.168.137.4
BOARD_DIR  := /home/root/edb_c/linking/

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean deploy flash

## Build → build/main
all: $(BUILD_DIR) $(TARGET)
	@echo " Build complete -> $(TARGET)"

## Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

## Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

## Compile each src/*.c -> build/*.o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

## Compile each ota/*.c -> build/*.o
$(BUILD_DIR)/%.o: $(OTA_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

## Compile vendored cJSON.c -> build/cJSON.o
$(BUILD_DIR)/%.o: $(VENDOR_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo " Cleaned"

## Copy binary to board
deploy:
	sshpass -e scp -o StrictHostKeyChecking=no $(TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/main.new
	sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) 'mv -f $(BOARD_DIR)/main.new $(BOARD_DIR)/main'
	@echo " Deployed to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

## Build + deploy in one shot
flash: clean all deploy