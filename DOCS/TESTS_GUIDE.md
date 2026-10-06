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

---

## 1. The test layers

Fast checks run first and catch most mistakes; the real board runs last and catches
what only hardware can show.

| Layer | Folder | CI job | Needs | Time | Count |
|---|---|---|---|---|---|
| Config validation + tool/firmware contract | `tests/validate/` | `validate-configs` | Python, gcc (host) | seconds | 4 files, 66 tests + 7 known issues |
| Compiler warnings | `tests/static/warnings_gate.py` | `compiler-warnings` | a clean build log | seconds | 1 gate |
| Tool unit tests | `tests/unit/tool/` | `unit-tool` | Python + headless PyQt5 | seconds | 8 files, 121 tests + 8 known issues |
| cppcheck, Doxygen | `tests/static/` | `cppcheck`, `doxygen` | — | — | step 4 |
| Firmware host tests (C) | `tests/host/` | `unit-firmware` | gcc, Unity, ASan/UBSan | — | step 5 |
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
| `tests/static/warnings_gate.py` | the clean build log has a `-Wall -Wextra` warning not in `compiler-warnings-baseline.json` (9 accepted) | [`CI_CD_GUIDE.md` §5.2](CI_CD_GUIDE.md#52-compiler-warnings--no-new-warnings). Fixed old ones: `--update-baseline` and commit |
| ELF check in `build-firmware` | `build/main` is not a 32-bit ARM EABI5 hard-float executable | `.gitlab/ci/build.yml` |

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
| Any C code | it must build without new warnings (`compiler-warnings`) |

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
