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
           $(SRC_DIR)/cloud/ota_logic.c          \
           $(SRC_DIR)/cloud/payload.c            \
           $(SRC_DIR)/cloud/config_push.c        \
           $(SRC_DIR)/cloud/mb_cmd.c             \
           $(SRC_DIR)/cloud/store_forward.c      \
           $(SRC_DIR)/system/connection.c        \
           $(SRC_DIR)/system/wifi.c              \
           $(SRC_DIR)/system/rtc.c               \
           $(SRC_DIR)/system/watchdog.c          \
           $(SRC_DIR)/ui/display.c               \
           $(SRC_DIR)/util/json.c                \
           $(SRC_DIR)/util/settings.c            \
           $(SRC_DIR)/util/paths.c               \
           $(SRC_DIR)/util/msg_queue.c           \
           $(SRC_DIR)/util/drive_logger.c

OBJS    := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# Subdirectories needed under build/
BUILD_SUBDIRS := $(BUILD_DIR)/fieldbus $(BUILD_DIR)/cloud $(BUILD_DIR)/system $(BUILD_DIR)/ui $(BUILD_DIR)/util

# --- Flags ---
# -Wall -Wextra: CI fails on any warning not in
# tests/static/compiler-warnings-baseline.json (DOCS/CI_CD_GUIDE.md)
WARNINGS := -Wall -Wextra

CFLAGS  := -O2 \
            $(WARNINGS) \
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
            -lm         \
            -Wl,--disable-new-dtags,-rpath,'$$ORIGIN/../lib'
# RPATH $ORIGIN/../lib: installed as /opt/gateway/bin/gateway, the binary loads the
# libraries bundled in /opt/gateway/lib first (DT_RPATH, also for their dependencies)
# and never needs anything copied into the OS's /usr/lib.
            

# --- Board install (DOCS/DEPLOYMENT.md) ---
# make install-board BOARD=root@192.168.1.26     (SSH key, or SSHPASS in the environment)
BOARD      ?=
SSH_OPTS   := -o StrictHostKeyChecking=accept-new
SSHPASS_E  := $(if $(SSHPASS),sshpass -e,)

# --- Package (deploy/install.sh installs it) ---
VERSION    := $(shell sed -n 's/^\#define APP_VERSION_DEF *"\(.*\)"/\1/p' $(INC_DIR)/settings.h)
PKG_DIR    := $(BUILD_DIR)/package
PKG_TAR    := $(BUILD_DIR)/gateway-$(VERSION).tar.gz

# --- Runtime shared libraries ---
# armhf libraries the binary needs (scripts/collect-libs.sh skips glibc and libgcc_s).
LIB_DIR     := $(BUILD_DIR)/lib

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean collect-libs package install-board flash

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

## Installable package: build/package/ and build/gateway-<version>.tar.gz
package: all
	rm -rf $(PKG_DIR) && mkdir -p $(PKG_DIR)/bin
	cp $(TARGET) $(PKG_DIR)/bin/gateway
	cp -a $(LIB_DIR) $(PKG_DIR)/lib
	cp deploy/gateway.service deploy/install.sh $(PKG_DIR)/
	echo "$(VERSION)" > $(PKG_DIR)/VERSION
	tar -C $(PKG_DIR) -czf $(PKG_TAR) .
	@echo " Package → $(PKG_TAR)"

## Copy the package to BOARD and run install.sh there (needs ssh/scp on this PC)
install-board: package
	@test -n "$(BOARD)" || (echo "usage: make install-board BOARD=root@<board-ip>" && false)
	$(SSHPASS_E) scp $(SSH_OPTS) $(PKG_TAR) $(BOARD):/tmp/gateway-pkg.tar.gz
	$(SSHPASS_E) ssh $(SSH_OPTS) $(BOARD) 'rm -rf /tmp/gateway-pkg && mkdir /tmp/gateway-pkg && tar -C /tmp/gateway-pkg -xzf /tmp/gateway-pkg.tar.gz && sh /tmp/gateway-pkg/install.sh && rm -rf /tmp/gateway-pkg /tmp/gateway-pkg.tar.gz'

## Build + install in one shot
flash: clean install-board
