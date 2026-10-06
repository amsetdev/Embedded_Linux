# Test Guide — STM32MP157F-DK2 gateway (Embedded_Linux)

For developers: **what every automated test checks, why it exists, how to run it,
and how to add one.** The pipeline itself (jobs, runner, reading results, secrets,
troubleshooting) is in [`CI_CD_GUIDE.md`](CI_CD_GUIDE.md); this guide is about the
tests inside the jobs.

Contents

1. [The test layers](#1-the-test-layers)
2. [Run the tests](#2-run-the-tests)
3. [Validate: configs and the tool/firmware contract](#3-validate-tests-testsvalidate)
4. [Build gates](#4-build-gates)
5. [Conventions every test follows](#5-conventions-every-test-follows)
6. [Which kind of test do I write?](#6-which-kind-of-test-do-i-write)
7. [Regression index: bug → test](#7-regression-index-bug--test)
8. [Desktop tool unit tests](#8-desktop-tool-unit-tests-testsunittool)
9. [Firmware host tests (C)](#9-firmware-host-tests-c-testshost)

---

## 1. The test layers

Fast checks run first and catch most mistakes; the real board runs last and catches
what only hardware can show.

| Layer | Folder | CI job | Needs | Time | Count |
|---|---|---|---|---|---|
| Config validation + tool/firmware contract | `tests/validate/` | `validate-configs` | Python, gcc (host) | seconds | 4 files, 66 tests + 7 known issues |
| Compiler warnings | `tests/static/warnings_gate.py` | `compiler-warnings` | a clean build log | seconds | 1 gate |
| Tool unit tests | `tests/unit/tool/` | `unit-tool` | Python + headless PyQt5 | seconds | 8 files, 121 tests + 8 known issues |
| cppcheck, Doxygen | `tests/static/` | `cppcheck`, `doxygen` | cppcheck 2.17 / doxygen 1.9.8 | ~1 min each | 2 gates (59 accepted findings; 0 doc warnings) |
| Firmware host tests (C) | `tests/host/` | `unit-firmware` | gcc, Unity, ASan/UBSan | ~30 s | 6 binaries, 67 tests (9 known issues) |
| Hardware in the loop | `tests/hil/` | `hil-tests` | the CI DK2 | — | step 7 |

`pytest` markers (`tests/pytest.ini`, `--strict-markers`) select the layers:
`validate`, `unit`, `hardware`. Every test file sets `pytestmark`.

---

## 2. Run the tests

Always from the repo root. The exact Docker commands CI uses are in
[`CI_CD_GUIDE.md` §3](CI_CD_GUIDE.md#3-run-everything-locally); the short versions:

```bash
# validate (needs gcc + tests/requirements.txt)
python3 -m pytest -c tests/pytest.ini tests/validate

# one test, verbose
python3 -m pytest -c tests/pytest.ini "tests/validate/test_negative_configs.py::test_rejected[label_64_bytes]" -v

# desktop tool (needs the tool's requirements.txt; Qt runs headless)
QT_QPA_PLATFORM=offscreen python3 -m pytest -c tests/pytest.ini tests/unit

# one config file
python3 tests/validate/validate_config.py configs/plant_a.json

# firmware C host tests (need gcc + make)
make -C tests/host run                    # binaries in build-host/, JUnit in reports/host-tests.xml
build-host/test_payload                   # one binary, Unity output

# static gates (need cppcheck / doxygen)
python3 tests/static/cppcheck_gate.py
python3 tests/static/doxygen_gate.py      # HTML in build-docs/html

# compiler warnings (after a CLEAN build that wrote build.log)
python3 tests/static/warnings_gate.py build.log
```

---

## 3. Validate tests (`tests/validate/`)

Checks the **contract between the desktop tool and the firmware** without a board:
every config file in the repo, the configs the tool builds, and that both sides
agree on the type names. A config the firmware would misread fails here in
seconds, before the build.

### 3.1 How: the real firmware parser on the PC

The firmware's JSON reader (`src/util/json.c`) looks each key up with `strstr()`
from the start of its section **to the end of the file**. A missing key is therefore
not "missing" but read from whatever comes later; strings stop at the first `"`;
numbers go through `atoi()`. Re-implementing that in Python would drift, so the
tests run **the firmware's own code**:

| File | Role |
|---|---|
| `fw_config_dump.c` | host harness: calls `settings_load()` + `parse_registers()` from the unmodified `src/util/settings.c`, `src/util/json.c`, `src/fieldbus/data.c` and writes every `AppSettings` field (with its `sizeof`) and every `ModbusPoint` as JSON. With no config dir: `settings_defaults()` only |
| `firmware_view.py` | compiles the harness with gcc **ASan + UBSan** (`-Werror`), caches it in `build-host/validate/` (rebuilt when any source or `inc/*.h` changes), runs it: `firmware_view(text)`, `firmware_defaults()` |
| `config_checks.py` | the rules (not a test file). Each check returns a list of problems, so one run reports everything wrong with a config. `check_firmware_reads()` compares the firmware's view with the file |
| `validate_config.py` | CLI for one or more files |

### 3.2 The test files

| File | What it checks |
|---|---|
| `test_config_files.py` | Every `configs/**/*.json`, root `smart_rtu_config*.json` and `tests/fixtures/configs/*.json` passes schema + rules + the firmware-read comparison; the schema itself is valid JSON Schema 2020-12; the known-good fixture exists. |
| `test_negative_configs.py` | 41 broken variants of `full_config.json` that must be rejected with a specific message, e.g. `wifi_enable_missing_read_from_modbus_tcp`, `register_slave_id_missing_taken_from_next`, `label_with_quote`, `label_utf8_64_bytes` (32 × `é`), `baud_as_string` (`atoi("\"9600\"")` = 0), `enable_as_bool`, `overlap_float_second_half`, `int32_at_65535`, `float_on_coil`, `topic_8_slashes`, `too_many_registers_2001`, `telemetry_too_large`. Plus 10 "must be allowed" cases: same address on another slave / table, exactly 2000 registers, a 63-byte label, no `ota`/`watchdog` sections, a register without `slave_id` when no later register has one. |
| `test_tool_contract.py` | Configs built with the tool's `DeviceConfig` (the code behind "Send to device"): all register/data types read correctly by the firmware; the CSV export carries the same content as the JSON. Strict known issues: Wi-Fi disabled with Modbus TCP, default-slave registers polled from the next register's slave, OTA topics fixed to `AMSET-001`, the tool's own validation too weak (3 cases). Also proves the rules reject those 3 cases. |
| `test_type_consistency.py` | `RegType` order in `inc/data.h`, the names `reg_type_from_string()` / `data_type_from_string()` recognise, the tool's `VALID_*` sets and UI dropdowns, the schema enums, and the codes the **real parser** produces for every (type, data_type) pair all agree. |

**Adding a device config**: put it in `configs/` (one file per device). CI validates it.

**Adding a rule**: add the check to `config_checks.py`, then a failing variant to
`NEGATIVE` in `test_negative_configs.py` (`"name": (mutation, "expected text in the
error")`) and, if the rule has an allowed edge case, an `ALLOWED` entry.

**The firmware reads a new key**: add it to `dump_settings()` in `fw_config_dump.c`
(one `STR()`/`INT()` line), the schema, and `full_config.json`. Until then the
comparison doesn't see the field.

---

## 4. Build gates

| Gate | Fails when | Details |
|---|---|---|
| `tests/static/warnings_gate.py` | the clean build log has a `-Wall -Wextra` warning not in `compiler-warnings-baseline.json` (8 accepted) | [`CI_CD_GUIDE.md` §5.2](CI_CD_GUIDE.md#52-compiler-warnings--no-new-warnings). Fixed old ones: `--update-baseline` and commit |
| ELF check in `build-firmware` | `build/main` is not a 32-bit ARM EABI5 hard-float executable | `.gitlab/ci/build.yml` |
| `tests/static/cppcheck_gate.py` | cppcheck finds anything not in `cppcheck-baseline.json` | [`CI_CD_GUIDE.md` §5.5](CI_CD_GUIDE.md#55-cppcheck--static-analysis-with-a-baseline) |
| `tests/static/doxygen_gate.py` | any doxygen warning, or the Doxyfile doesn't cover exactly the makefile's `SRCS` | [`CI_CD_GUIDE.md` §5.6](CI_CD_GUIDE.md#56-doxygen--documentation-gate) |

### 4.1 Doxygen style used in this repo

Copy it for new code:

```c
/**
 * @brief One sentence: what it does.
 *
 * Optional details: when it is called, what it must not do, units, locking.
 *
 * @param slave_id Modbus slave address (1-247).
 * @param addr     Register address.
 * @param value    Value to write.
 * @return 1 on success, 0 on failure.
 */
int data_write_register(int slave_id, RegType reg_type, uint16_t addr, uint16_t value);
```

* **Public functions: in the header only.** The definition in the `.c` file may keep a
  plain `/* … */` comment; a second `/** … */` block gives "multiple @param
  documentation sections".
* Static functions, file-local variables and macros: at the definition in the `.c` file.
* Struct members, enum values and short macros: trailing `/**< … */`.
* Every `@param` and the `@return` (unless `void`).
* Describe what the code **does** today, including limits and "not called" where true.
* Copied third-party definitions (like the DRM UAPI structs in `display.c`) go between
  `/** @cond NAME` … `/** @endcond */` with a note where they come from.

---

## 5. Conventions every test follows

* **A bug fix comes with a test that fails without the fix.** Start its docstring or
  comment with `Regression:` and say what used to happen (see §7).
* **Known issues stay in the suite and fail once fixed** (strict xfail):
  `@pytest.mark.xfail(strict=True, reason="Known issue: …")`. When it starts failing
  with `XPASS(strict)`, remove the marker and make it a normal test. The open list is
  in `CI_CD_GUIDE.md` §6.
* **Test the real code, not a copy of it**: the validate tests compile the firmware's
  parser instead of re-implementing it; tool tests import the tool's modules.
* **Assert the contract, not the implementation**: what the firmware ends up with,
  what the tool writes, what arrives at the slave / broker.
* **Test names say what must be true**: `test_wifi_stays_enabled_when_modbus_tcp_disabled`,
  not `test_wifi_2`.
* **No secrets in tests or fixtures**: fixture passwords and hosts are obviously fake
  (`not-a-real-password`, `example-ats.iot…`).

---

## 6. Which kind of test do I write?

| You changed… | Write |
|---|---|
| A config rule, a buffer size the tool must respect | `tests/validate/` (rule + negative case) |
| A key the firmware reads (`settings_load()`, `parse_registers()`) | `fw_config_dump.c` + schema + fixture (§3.2) |
| Register / data type names | all of: `inc/data.h`, `data.c`, tool tables, schema; `test_type_consistency.py` tells you what's missing |
| The desktop tool (model, import, transports, workers, window) | `tests/unit/tool/` (§8) |
| A delivery mode (SSH / serial / MQTT) | transport test against the matching fake, plus a worker test for what the button does |
| Firmware logic without hardware calls (parsing, formatting, decisions, ordering) | a module in `src/` without library includes + `tests/host/` (§9) |
| Firmware that talks to Modbus, MQTT, the file system or curl | move its decisions into such a module (ops struct for the side effects); the rest is covered by HIL (step 7) |
| Any C code | it must build without new warnings (`compiler-warnings`) and without new cppcheck findings |
| Any C function, struct, member, macro or global | its Doxygen comment (§4.1; the `doxygen` job fails otherwise) |

---

## 7. Regression index: bug → test

Bugs found while building this suite, and the test that keeps each one visible.
Open ones are strict xfails (they fail once fixed, then become regressions).

| Bug | Status | Guarded by |
|---|---|---|
| Wi-Fi disabled whenever Modbus TCP is disabled (tool omits `wifi.enable`) | open | `test_tool_contract.py::test_wifi_stays_enabled_when_modbus_tcp_disabled`, negative `wifi_enable_missing_read_from_modbus_tcp` |
| Register on the default slave polled from the next register's slave | open | `test_tool_contract.py::test_register_with_device_default_slave_polled_from_device_slave`, negative `register_slave_id_missing_taken_from_next` |
| All gateways use OTA topics `devices/AMSET-001/ota/*` | open | `test_tool_contract.py::test_ota_topics_follow_configured_device_id` |
| Tool accepts labels the firmware truncates/misparses, overlapping 32-bit registers | open | `test_tool_contract.py::test_tool_rejects_what_firmware_mishandles` |
| MQTT delivery: firmware doesn't subscribe to the tool's config topic | open | `test_tool_contract.py::test_firmware_subscribes_to_tool_config_topic` |
| Serial delivery: JSON not written (heredoc never ends) but reported as success | open | `test_serial_transport.py::test_push_json_writes_file`, `::test_failed_write_not_reported_as_success` |
| Serial Read returns the echoed command, not the file | open | `test_serial_transport.py::test_read_text_with_echo` |
| Excel `INT16` imported as `int32` | open | `test_excel_import.py::test_int16_not_imported_as_int32` |
| Offline queue overwrites same-second pushes; keeps only the CSV | open | `test_offline_queue_credentials.py` (2 tests) |
| OTA app update installed unverified when `sha256` is missing | open | `test_ota_logic.c::test_update_without_sha256_not_installed` |
| OTA accepts any URL scheme; long URLs cut; `\/` not decoded | open | `test_ota_logic.c` (3 tests) |
| Telemetry `"x":nan` (invalid JSON) for NaN floats | open | `test_payload.c::test_nan_float_gives_valid_json` |
| Offline files overwritten within one second; deleted without broker confirmation; backlog never drains | open | `test_store_forward.c` (3 tests) |
| `json.c` doesn't unescape `\"` | open | `test_json.c::test_escaped_quote_in_value` |

---

## 8. Desktop tool unit tests (`tests/unit/tool/`)

The PyQt5 tool runs **headless** (`QT_QPA_PLATFORM=offscreen`). Nothing reaches
real hardware, network or your keyring: see the safety notes in
[`CI_CD_GUIDE.md` §5.4](CI_CD_GUIDE.md#54-unit-tool--desktop-tool-tests-python-headless-pyqt5).

### 8.1 Fixtures and fakes

| Name | Where | Use |
|---|---|---|
| `qapp`, `window` | `conftest.py` | the QApplication; a fresh `MainWindow` (closed after the test) |
| `dialogs` | `conftest.py` | records every `QMessageBox`/`QFileDialog`; set `answer`, `open_path`, `save_path` |
| `memory_keyring`, `queue_dir` | `conftest.py` (autouse) | in-memory OS keyring (`.store`), temp offline queue |
| `FakeBoardFS` + `ssh_client_factory` | `fakes.py` | the board over SSH: files dict, runs `mv`/`rm`/`tail`, records commands and connections |
| `FakeConsole` | `fakes.py` | a login shell on the serial port: login/password prompts, **echo** (default on, like a real TTY), heredoc, `mv`/`rm`/`cat`/`echo`, `&&` chains; `fail_mv=True` makes writes fail |
| `FakeBroker` | `fakes.py` | MQTT broker: retained messages, CONNACK codes, `responders` that reply on an ack topic |
| `FakeTransport` | `fakes.py` | records calls; `results[method]` sets what a call returns |
| `FakeClock` | `fakes.py` | replaces a module's `time`: `sleep()` advances `time()` instantly |

### 8.2 The test files

| File | What it checks |
|---|---|
| `test_register_model.py` | Point validation edges (label, address 0–65535, slave 0–247, types); `validate_all()` reports every bad row and exact duplicates, but not the same address on another slave; the combined config has every section the firmware reads; `slave_id` omitted for "device default"; JSON/CSV files; **project file round trip of every field**; older project files; ESP32 NVS export import. |
| `test_excel_import.py` | Minimal Label/Address sheet; header aliases; missing required column is an error; register/data type synonyms; unknown types default with one warning each; bad rows skipped with the right Excel row numbers; blank rows ignored; imported points valid. Known issue: `INT16` → `int32`. |
| `test_ssh_transport.py` | Connect parameters; failures reported, not raised; write to `.tmp` then `mv` (atomic); rename failure; both config files; stop after the first failure; binary upload; read / missing file; delete; restart command; log tail. |
| `test_serial_transport.py` | Login with username/password; writes via heredoc with echo off; the CSV with echo on; failure detection without echo; read; delete; port closed afterwards. Known issues: JSON not written, failures reported as success, Read returns the echo. |
| `test_mqtt_transport.py` | TLS and credentials; CONNACK refusals explained; CONNACK timeout; success **only with an ack**; no ack → failure but retained; CSV + JSON topics; read retained; delete clears retained; no file upload. |
| `test_offline_queue_credentials.py` | Queue enqueue/list/remove, corrupt files skipped, oldest first; keyring save/read/clear. Known issues: same-second overwrite, CSV-only queue. |
| `test_workers.py` | What Push / Read / Send / Erase / Retry do per mode: SSH push then restart (not on failure), serial login passed through, MQTT failure queues the config, certificates uploaded first (failure stops), MQTT says certs must be placed manually, exceptions become failures, read/erase targets per mode, "Send Settings" doesn't restart, retry removes only delivered entries. |
| `test_main_window.py` | Device page ↔ config round trip; default device_id; board cert paths written (not PC paths); table ↔ points; invalid rows block validation/push with a dialog; push uses the selected mode and the full config; passwords saved to the keyring only when "Remember" is ticked and prefilled on start; save/load project; broken project; cancelled dialogs change nothing; export writes `.csv` + `.json`; Excel replace/append/cancel; delete rows; retry needs the MQTT tab; "Erase All Data" asks first and clears queue + keyring; status log escapes HTML. |

**Adding a test**: drive the real widget or function; answer dialogs with `dialogs`;
for anything that talks to the board use the fakes, never a real port or host.

---

## 9. Firmware host tests (C, `tests/host/`)

Firmware logic that doesn't need libmodbus, libmosquitto, libcurl or OpenSSL is
compiled for the PC with Unity, AddressSanitizer and UndefinedBehaviorSanitizer.
These tests are exact and fast, and they cover cases the board can't produce on
demand (a broker that never confirms, a full queue, a failed rename during OTA).
The modules and why they exist: [`CI_CD_GUIDE.md` §5.7](CI_CD_GUIDE.md#57-unit-firmware--firmware-c-unit-tests-host).

| Binary | Module under test | What it checks |
|---|---|---|
| `test_json` | `src/util/json.c` | String/int values, whitespace and newlines, missing keys leave the value unchanged, non-string rejected, truncation to the buffer, keys matched with their quotes (`"id"` ≠ `"device_id"`), `atoi` turns `"9600"`/`true` into 0, the search continues past the object (what the config format relies on), `*_from` = plain, `read_file` (content, empty, missing). Known issue: no unescaping. |
| `test_payload` | `src/cloud/payload.c` | Empty message; every data type (`w`, `d`, `f` with 2 decimals, `b`); failed reads left out; no stray comma; a full buffer stops adding values and stays valid. Known issue: NaN → `nan`. |
| `test_store_forward` | `src/cloud/store_forward.c` | File named `<unix s>000.txt`; NULL / write failure; only `*.txt` are payloads; offline → nothing; oldest first, then deleted; unreadable file kept; list/remove failures reported. Known issues: same-second overwrite, deleted without broker confirmation, backlog never drains. |
| `test_ota_logic` | `src/cloud/ota_logic.c` | App and system commands parsed (sha256 only for app); other topics / empty payload ignored; missing URL; exact status JSON; digest check (case-insensitive, wrong length); the app-update sequence step by step: happy path, download failure, SHA mismatch deletes the download, first install without a running binary, backup failure keeps the running binary, replace failure restores the backup. Known issues: no sha256 → unverified install, any URL scheme, long URL cut, `\/` not decoded. |
| `test_msg_queue` | `src/util/msg_queue.c` | FIFO order; payload copied; full queue drops the oldest; wrap-around; invalid arguments; items still delivered after shutdown; shutdown and push wake a blocked consumer (threads); destroy frees pending items (LeakSanitizer). |
| `test_data_read` | `src/fieldbus/data.c` (+ `settings.c`, `json.c`) with a fake fieldbus driver | uint16 reads; each table read from its own Modbus table; float32 and int32 **high word first**; signed int32; int32 above 2^24 exact; slave switched per point; retries (`MAX_RETRIES`) then success; failure marks the point invalid; no driver → invalid; register and block writes. |

**Adding a test**

1. The code must be in a library-free module (see `CI_CD_GUIDE.md` §7 "New firmware
   logic you want tested"); side effects go through an ops struct so the test can fake them.
2. `static void test_<what_must_be_true>(void)` + `RUN_TEST(...)` in `tests/host/test_<module>.c`;
   `setUp`/`tearDown` reset the fakes.
3. New binary: `FW_<name> := <sources>` and `<name>` in `TESTS` in `tests/host/Makefile`.
4. A bug that isn't fixed yet: `KNOWN_ISSUE(correct_condition, "why")` from
   `tests/host/known_issue.h` (reported as skipped; fails with "now FIXED" once it is).

Everything compiles with `-Werror`; the sanitizers abort on the first error, so an
out-of-bounds read fails the test even if the result looks right.
