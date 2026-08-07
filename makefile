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
WIFI_TARGET := $(BUILD_DIR)/wifi_service

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

# --- wifi service sources (shares wifi/ethernet/network_manager/settings) ---
WIFI_SRCS := $(SRC_DIR)/wifi_main.c \
             $(SRC_DIR)/wifi.c \
             $(SRC_DIR)/network_manager.c \
             $(SRC_DIR)/ethernet.c \
             $(SRC_DIR)/settings.c \
             $(SRC_DIR)/json.c

# --- ota service sources ---
OTA_SRCS := $(OTA_DIR)/ota_main.c \
            $(OTA_DIR)/ota.c \
            $(OTA_DIR)/ota_config.c \
            $(OTA_DIR)/ota_network.c \
            $(OTA_DIR)/ota_install.c \
            $(VENDOR_DIR)/cJSON.c

# separate object dirs per target -- wifi.c/network_manager.c/ethernet.c/
# settings.c/json.c are compiled twice (once for main, once for
# wifi_service), so they need separate .o output dirs or the two
# targets would clobber each other's object files.
MAIN_OBJDIR := $(BUILD_DIR)/main_obj
WIFI_OBJDIR := $(BUILD_DIR)/wifi_obj
OTA_OBJDIR  := $(BUILD_DIR)/ota_obj

MAIN_OBJS := $(patsubst %.c,$(MAIN_OBJDIR)/%.o,$(notdir $(MAIN_SRCS)))
WIFI_OBJS := $(patsubst %.c,$(WIFI_OBJDIR)/%.o,$(notdir $(WIFI_SRCS)))
OTA_OBJS  := $(patsubst %.c,$(OTA_OBJDIR)/%.o,$(notdir $(OTA_SRCS)))

CFLAGS := -O2 -I inc -I include -I ota -I ota/vendor

LDFLAGS_MAIN := -L/usr/lib/arm-linux-gnueabihf \
                -lmodbus -lmosquitto -lsqlite3 -lpthread \
                -lssl -lcrypto -lcurl -ldl -lz -lm

LDFLAGS_OTA := -L/usr/lib/arm-linux-gnueabihf \
               -lmosquitto -lpthread -lssl -lcrypto -lcurl -ldl -lz -lm

LDFLAGS_WIFI := -L/usr/lib/arm-linux-gnueabihf -lpthread

BOARD_USER := root
BOARD_IP   := 192.168.137.4
BOARD_DIR  := /home/root/edb_c/linking/

.PHONY: all clean deploy flash

all: $(MAIN_OBJDIR) $(WIFI_OBJDIR) $(OTA_OBJDIR) $(MAIN_TARGET) $(OTA_TARGET) $(WIFI_TARGET)
	@echo " Build complete -> $(MAIN_TARGET), $(OTA_TARGET) & $(WIFI_TARGET)"

$(MAIN_OBJDIR) $(WIFI_OBJDIR) $(OTA_OBJDIR):
	mkdir -p $@

$(MAIN_TARGET): $(MAIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_MAIN)

$(OTA_TARGET): $(OTA_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_OTA)

$(WIFI_TARGET): $(WIFI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_WIFI)

$(MAIN_OBJDIR)/%.o: $(SRC_DIR)/%.c | $(MAIN_OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(WIFI_OBJDIR)/%.o: $(SRC_DIR)/%.c | $(WIFI_OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OTA_OBJDIR)/%.o: $(OTA_DIR)/%.c | $(OTA_OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OTA_OBJDIR)/%.o: $(VENDOR_DIR)/%.c | $(OTA_OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
	@echo " Cleaned"

deploy:
	sshpass -e scp -o StrictHostKeyChecking=no $(MAIN_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/main.new
	sshpass -e scp -o StrictHostKeyChecking=no $(OTA_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/ota_service.new
	sshpass -e scp -o StrictHostKeyChecking=no $(WIFI_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/wifi_service.new
	sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) '\
	  mv -f $(BOARD_DIR)/main.new $(BOARD_DIR)/main && \
	  mv -f $(BOARD_DIR)/ota_service.new $(BOARD_DIR)/ota_service && \
	  mv -f $(BOARD_DIR)/wifi_service.new $(BOARD_DIR)/wifi_service && \
	  chmod +x $(BOARD_DIR)/main $(BOARD_DIR)/ota_service $(BOARD_DIR)/wifi_service'
	@echo " Deployed to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

flash: clean all deploy