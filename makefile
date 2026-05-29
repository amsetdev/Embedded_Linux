# ============================================================
#  STM32MP1 Cross-Compilation Makefile
#  Target : arm-linux-gnueabihf
#  Host   : WSL (Ubuntu)
# ============================================================

# --- Toolchain ---
CC      := arm-linux-gnueabihf-gcc

# --- Project ---
TARGET  := src/main
SRC     := src/main.c

# --- Flags ---
CFLAGS  := -O2 \
            -I inc \
            -I include

LDFLAGS := -L/usr/lib/arm-linux-gnueabihf \
            -lmodbus    \
            -lmosquitto \
            -lsqlite3   \
            -lpthread   \
            -lssl       \
            -lcrypto    \
            -ldl        \
            -lz         \
            -lm

# --- Board Deploy ---
BOARD_USER := root
BOARD_IP   := 192.168.1.104
BOARD_DIR  := /home/root/edb_c/

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean deploy flash

## Build  →  src/main
all:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "✓ Build complete → $(TARGET)"

## Remove binary
clean:
	rm -f $(TARGET)
	@echo "✓ Cleaned"

## Copy binary to board
deploy:
	scp $(TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)
	@echo "✓ Deployed to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)"

## Build + deploy in one shot
flash: all deploy