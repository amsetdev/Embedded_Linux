# ============================================================
#  STM32MP1 Cross-Compilation Makefile
# ============================================================

CC := arm-linux-gnueabihf-gcc

SRC_DIR    := src
INC_DIR    := inc
BUILD_DIR  := build
OTA_DIR    := ota
VENDOR_DIR := ota/vendor/cjson

MAIN_TARGET := $(BUILD_DIR)/main
OTA_TARGET  := $(BUILD_DIR)/ota_service

# --- main app sources (OTA removed) ---
MAIN_SRCS := $(SRC_DIR)/main.c \
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
             $(SRC_DIR)/json.c

# --- ota service sources ---
OTA_SRCS := $(OTA_DIR)/ota_main.c \
            $(OTA_DIR)/ota.c \
            $(OTA_DIR)/ota_config.c \
            $(OTA_DIR)/ota_network.c \
            $(OTA_DIR)/ota_install.c \
            $(VENDOR_DIR)/cJSON.c

MAIN_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(MAIN_SRCS)))
OTA_OBJS  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(OTA_SRCS)))

CFLAGS := -O2 -I inc -I include -I ota -I ota/vendor

LDFLAGS_MAIN := -L/usr/lib/arm-linux-gnueabihf \
                -lmodbus -lmosquitto -lsqlite3 -lpthread \
                -lssl -lcrypto -lcurl -ldl -lz -lm

LDFLAGS_OTA := -L/usr/lib/arm-linux-gnueabihf \
               -lmosquitto -lpthread -lssl -lcrypto -lcurl -ldl -lz -lm

BOARD_USER := root
BOARD_IP   := 192.168.137.150
BOARD_DIR  := /home/root/edb_c/linking/

.PHONY: all clean deploy flash

all: $(BUILD_DIR) $(MAIN_TARGET) $(OTA_TARGET)
	@echo " Build complete -> $(MAIN_TARGET) & $(OTA_TARGET)"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(MAIN_TARGET): $(MAIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_MAIN)

$(OTA_TARGET): $(OTA_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_OTA)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(OTA_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(VENDOR_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
clean:
	rm -rf $(BUILD_DIR)
	@echo " Cleaned"

deploy:
	sshpass -e scp -o StrictHostKeyChecking=no $(MAIN_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/main.new
	sshpass -e scp -o StrictHostKeyChecking=no $(OTA_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/ota_service.new
	sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) '\
	  mv -f $(BOARD_DIR)/main.new $(BOARD_DIR)/main && \
	  mv -f $(BOARD_DIR)/ota_service.new $(BOARD_DIR)/ota_service && \
	  chmod +x $(BOARD_DIR)/main $(BOARD_DIR)/ota_service'
	@echo " Deployed to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

flash: clean all deploy