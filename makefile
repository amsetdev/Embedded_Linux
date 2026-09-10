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

SRCS    := $(SRC_DIR)/main.c                    \
           $(SRC_DIR)/fieldbus/fieldbus_rtu.c    \
           $(SRC_DIR)/fieldbus/fieldbus_tcp.c    \
           $(SRC_DIR)/fieldbus/data.c            \
           $(SRC_DIR)/fieldbus/mb_tcp.c          \
           $(SRC_DIR)/cloud/mqtt.c               \
           $(SRC_DIR)/cloud/https.c              \
           $(SRC_DIR)/cloud/storage.c            \
           $(SRC_DIR)/cloud/ota.c                \
           $(SRC_DIR)/system/connection.c        \
           $(SRC_DIR)/system/wifi.c              \
           $(SRC_DIR)/system/rtc.c               \
           $(SRC_DIR)/system/watchdog.c          \
           $(SRC_DIR)/ui/display.c               \
           $(SRC_DIR)/util/json.c                \
           $(SRC_DIR)/util/settings.c            \
           $(SRC_DIR)/util/msg_queue.c           \
           $(SRC_DIR)/util/drive_logger.c

OBJS    := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# Subdirectories needed under build/
BUILD_SUBDIRS := $(BUILD_DIR)/fieldbus $(BUILD_DIR)/cloud $(BUILD_DIR)/system $(BUILD_DIR)/ui $(BUILD_DIR)/util

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
BOARD_IP   := 192.168.1.24
BOARD_DIR  := /home/root/edb_c/linking/
BOARD_LIB_DIR := /usr/lib

# --- Runtime shared libraries to deploy ---
# These are the armhf .so files the binary needs at runtime.
# System libs (pthread, m, dl, z) are already on the board.
SYSROOT_LIB := /usr/lib/arm-linux-gnueabihf
LIB_DIR     := $(BUILD_DIR)/lib

DEPLOY_LIBS := libmodbus.so.5    \
               libmosquitto.so.1 \
               libsqlite3.so.0   \
               libssl.so.3       \
               libcrypto.so.3    \
               libcurl.so.4

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean deploy deploy-libs flash collect-libs

## Build → build/main + build/lib/*.so
all: $(BUILD_DIR) $(BUILD_SUBDIRS) $(TARGET) collect-libs
	@echo " Build complete → $(TARGET)"

## Create build directory and subdirectories
$(BUILD_DIR) $(BUILD_SUBDIRS):
	mkdir -p $@

## Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

## Compile each src/**/*.c → build/**/*.o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

## Collect ALL armhf runtime .so files (including transitive deps) into build/lib/
collect-libs:
	mkdir -p $(LIB_DIR)
	@echo "  Resolving full dependency tree…"
	bash ./scripts/collect-libs.sh $(TARGET) $(LIB_DIR)
	@echo "  Libraries collected in $(LIB_DIR)/"

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo " Cleaned"

## Copy binary to board
deploy:
	sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)'
	sshpass -e scp -o StrictHostKeyChecking=no $(TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/main.new
	sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) 'mv -f $(BOARD_DIR)/main.new $(BOARD_DIR)/main'
	@echo " Deployed binary to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

## Copy runtime .so files to board and run ldconfig
deploy-libs:
	@if [ -d "$(LIB_DIR)" ] && [ "$$(ls -A $(LIB_DIR) 2>/dev/null)" ]; then \
		echo " Deploying shared libraries…"; \
		sshpass -e scp -o StrictHostKeyChecking=no $(LIB_DIR)/*.so* $(BOARD_USER)@$(BOARD_IP):$(BOARD_LIB_DIR)/; \
		sshpass -e ssh -o StrictHostKeyChecking=no $(BOARD_USER)@$(BOARD_IP) 'ldconfig'; \
		echo " Libraries deployed and ldconfig updated"; \
	else \
		echo " No libraries to deploy (run 'make all' first)"; \
	fi

## Build + deploy everything in one shot
flash: clean all deploy-libs deploy