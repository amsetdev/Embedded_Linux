# Host-side C unit tests (DOCS/TESTS_GUIDE.md §9): the firmware's own sources,
# compiled for the PC with native gcc, Unity, AddressSanitizer + UndefinedBehaviorSanitizer.
#
#   make -C tests/host                 build every test into build-host/
#   make -C tests/host run             build + run all (JUnit: reports/host-tests.xml)
#
# Each test binary = tests/host/test_<name>.c + the firmware files listed for it
# below. Those files must not need libmodbus, libmosquitto, libcurl or OpenSSL.

REPO    := $(abspath $(CURDIR)/../..)
OUT     ?= $(REPO)/build-host
CC      ?= gcc
SAN     := -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS  := -std=gnu11 -g -O1 -Wall -Wextra -Werror $(SAN) \
           -I$(REPO)/inc -I$(CURDIR) -I$(CURDIR)/unity
LDFLAGS := $(SAN) -pthread

SRC := $(REPO)/src

# test name -> firmware sources under test
FW_json          := $(SRC)/util/json.c
FW_payload       := $(SRC)/cloud/payload.c
FW_store_forward := $(SRC)/cloud/store_forward.c
FW_ota_logic     := $(SRC)/cloud/ota_logic.c $(SRC)/util/json.c
FW_config_push   := $(SRC)/cloud/config_push.c $(SRC)/util/json.c
FW_msg_queue     := $(SRC)/util/msg_queue.c
FW_data_read     := $(SRC)/fieldbus/data.c $(SRC)/util/settings.c $(SRC)/util/json.c $(SRC)/util/paths.c
FW_paths         := $(SRC)/util/paths.c
FW_mb_cmd        := $(SRC)/cloud/mb_cmd.c $(SRC)/util/json.c

TESTS := json payload store_forward ota_logic config_push msg_queue data_read paths mb_cmd
BINS  := $(addprefix $(OUT)/test_,$(TESTS))

.PHONY: all run clean
all: $(BINS)

$(OUT)/unity.o: $(CURDIR)/unity/unity.c | $(OUT)
	$(CC) -std=gnu11 -g -O1 $(SAN) -I$(CURDIR)/unity -c -o $@ $<

$(OUT):
	mkdir -p $@

.SECONDEXPANSION:
$(OUT)/test_%: $(CURDIR)/test_%.c $$(FW_$$*) $(OUT)/unity.o $(wildcard $(REPO)/inc/*.h) $(CURDIR)/known_issue.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(FW_$*) $(OUT)/unity.o $(LDFLAGS)

run: all
	python3 $(CURDIR)/run_tests.py $(OUT) $(REPO)/reports/host-tests.xml

clean:
	rm -f $(BINS) $(OUT)/unity.o
