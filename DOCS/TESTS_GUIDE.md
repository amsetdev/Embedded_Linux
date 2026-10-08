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
10. [Release script tests](#10-release-script-tests-testsunitrelease)
11. [Hardware-in-the-loop tests](#11-hardware-in-the-loop-tests-testshil)

---

## 1. The test layers

Fast checks run first and catch most mistakes; the real board runs last and catches
what only hardware can show.

| Layer | Folder | CI job | Needs | Time | Count |
|---|---|---|---|---|---|
| Config validation + tool/firmware contract | `tests/validate/` | `validate-configs` | Python, gcc (host) | seconds | 4 files, 77 tests |
| Compiler warnings | `tests/static/warnings_gate.py` | `compiler-warnings` | a clean build log | seconds | 1 gate |
| Tool unit tests | `tests/unit/tool/` | `unit-tool` | Python + headless PyQt5 | seconds | 8 files, 163 tests |
| Release script | `tests/unit/release/` | `unit-tool` | Python | seconds | 1 file, 20 tests |
| cppcheck, Doxygen | `tests/static/` | `cppcheck`, `doxygen` | cppcheck 2.17 / doxygen 1.9.8 | ~1 min each | 2 gates (50 accepted findings; 0 doc warnings) |
| Firmware host tests (C) | `tests/host/` | `unit-firmware` | gcc, Unity, ASan/UBSan | ~30 s | 7 binaries, 83 tests |
| Hardware in the loop | `tests/hil/` | `hil-tests` | the CI DK2 (+ RS485 adapter, + AWS for 6 tests) | ~10 min | 7 files, 30 tests |

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

The firmware's JSON reader (`src/util/json.c`) is hand-written: numbers go through
`atoi()`, strings are cut to their buffer, and before the bug-fix pass a missing key
was read from a later section. Re-implementing it in Python would drift, so the tests
run **the firmware's own code**:

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
| `test_negative_configs.py` | 39 broken variants of `full_config.json` that must be rejected with a specific message, e.g. `label_with_quote`, `label_utf8_64_bytes` (32 × `é`), `baud_as_string` (`atoi("\"9600\"")` = 0), `enable_as_bool`, `overlap_float_second_half`, `int32_at_65535`, `float_on_coil`, `topic_8_slashes`, `too_many_registers_2001`, `telemetry_too_large`. Plus 12 "must be allowed" cases: same address on another slave / table, exactly 2000 registers, a 63-byte label, no `ota`/`watchdog` sections, and the regressions `wifi_without_enable_keeps_default` / `register_without_slave_id_uses_device_slave` (a missing key keeps its default instead of being read from a later section). |
| `test_tool_contract.py` | Configs built with the tool's `DeviceConfig` (the code behind "Send to device"): all register/data types read correctly by the firmware; the CSV export carries the same content as the JSON. Regressions: Wi-Fi stays enabled with Modbus TCP off (and can be disabled); default-slave registers polled from the device's slave (tool and firmware side); OTA topics follow the device ID (an `ota` section still overrides); escaped strings read like JSON; the tool rejects labels > 63 bytes / with `"` and overlapping registers; firmware and tool agree on the MQTT config topics. |
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
| Release packaging (`tools/release/make_release.py`) | `tests/unit/release/` (§10) |
| A delivery mode (SSH / serial / MQTT) | transport test against the matching fake, plus a worker test for what the button does |
| Firmware logic without hardware calls (parsing, formatting, decisions, ordering) | a module in `src/` without library includes + `tests/host/` (§9) |
| Firmware that talks to Modbus, MQTT, the file system or curl | move its decisions into such a module (ops struct for the side effects), and a HIL test for the real thing (§11) |
| Installation, service, paths (`deploy/`) | HIL `test_01_install.py` |
| Any C code | it must build without new warnings (`compiler-warnings`) and without new cppcheck findings |
| Any C function, struct, member, macro or global | its Doxygen comment (§4.1; the `doxygen` job fails otherwise) |

---

## 7. Regression index: bug → test

Bugs found while building this suite, and the test that keeps each one visible.
Open ones are strict xfails (they fail once fixed, then become regressions).

| Bug | Status | Guarded by |
|---|---|---|
| Wi-Fi disabled whenever Modbus TCP is disabled (tool omitted `wifi.enable`, firmware read the next section) | fixed | `test_tool_contract.py::test_wifi_stays_enabled_when_modbus_tcp_disabled`, ALLOWED `wifi_without_enable_keeps_default`, `test_register_model.py::test_wifi_enable_and_country_written` |
| Register on the default slave polled from the next register's slave | fixed | `test_tool_contract.py::test_register_with_device_default_slave_polled_from_device_slave`, `::test_firmware_uses_device_slave_when_register_slave_missing`, `test_register_model.py::test_register_slave_id_always_written` |
| All gateways used OTA topics `devices/AMSET-001/ota/*` | fixed | `test_tool_contract.py::test_ota_topics_follow_configured_device_id` |
| Tool accepted labels the firmware cuts/misparses and overlapping 32-bit registers | fixed | `test_tool_contract.py::test_tool_rejects_what_firmware_mishandles`, `test_register_model.py` |
| MQTT delivery: firmware didn't subscribe to the tool's config topic; tool sent the CSV | fixed | `test_tool_contract.py::test_firmware_subscribes_to_tool_config_topic`, `test_config_push.c`, `test_mqtt_transport.py::test_push_combined_config_sends_the_json_the_firmware_reads` |
| Serial delivery: JSON not written (heredoc never ended) but reported as success | fixed | `test_serial_transport.py::test_push_json_writes_file`, `::test_failed_write_not_reported_as_success` |
| Serial Read returned the echoed command | fixed | `test_serial_transport.py::test_read_text_with_echo` |
| Excel `INT16` imported as `int32` | fixed | `test_excel_import.py::test_int16_not_imported_as_int32` |
| Tool offline queue overwrote same-second pushes; kept only the CSV | fixed | `test_offline_queue_credentials.py` (2 tests), `test_workers.py::test_retry_queue_sends_json_and_removes_only_delivered` |
| OTA app update installed unverified without `sha256` | fixed | `test_ota_logic.c::test_app_request_needs_valid_sha256`, `::test_update_without_sha256_not_installed` |
| OTA accepted any URL scheme; long URLs cut; `\/` not decoded | fixed | `test_ota_logic.c::test_non_https_url_rejected`, `::test_long_presigned_url_not_truncated`, `::test_escaped_slashes_in_url` |
| Telemetry `"x":nan` (invalid JSON); `(int)NaN` undefined behaviour | fixed | `test_payload.c::test_nan_float_gives_valid_json`, `test_data_read.c::test_nan_and_huge_float_converted_without_ub` |
| Offline files overwritten within one second; deleted without broker confirmation; backlog never drained | fixed | `test_store_forward.c::test_two_stores_in_one_second_both_kept`, `::test_file_kept_when_publish_not_confirmed`, `::test_backlog_drains` |
| `json.c` didn't unescape `\"` | fixed | `test_json.c::test_escaped_quote_in_value`, `test_tool_contract.py::test_escaped_strings_read_like_json` |

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
| `test_register_model.py` | Point validation edges (label, address 0–65535, slave 0–247, types, 32-bit limits); label bytes and `"`/`\\`; `validate_all()` reports every bad row, overlapping 32-bit registers (device default slave counted), duplicate labels, but not the same address on another slave / table; `wifi.enable`/`country` and every `slave_id` written; the combined config has every section the firmware reads; `slave_id` omitted for "device default"; JSON/CSV files; **project file round trip of every field**; older project files; ESP32 NVS export import. |
| `test_excel_import.py` | Minimal Label/Address sheet; header aliases; missing required column is an error; register/data type synonyms; unknown types default with one warning each; bad rows skipped with the right Excel row numbers; blank rows ignored; imported points valid; IEC 61131 names (`INT`/`UINT`/`WORD` 16-bit, `DINT`/`UDINT` 32-bit, `REAL`) with warnings for signed 16 / unsigned 32. Regression: `INT16` was imported as `int32`. |
| `test_ssh_transport.py` | Connect parameters; failures reported, not raised; write to `.tmp` then `mv` (atomic); rename failure; both config files; stop after the first failure; binary upload; read / missing file; delete; restart command; log tail. |
| `test_serial_transport.py` | Login with username/password; writes via heredoc with and without echo; content containing an `EOF` line; failure detection; read and delete with and without echo; port closed afterwards. Regressions: JSON not written, failures reported as success, Read returned the echo. |
| `test_mqtt_transport.py` | TLS and credentials; client-certificate auth (AWS IoT) without username; CONNACK refusals explained; CONNACK timeout; success **only with an ack** `saved`/`unchanged`, `rejected`/`error` is a failure; no ack → failure but retained; the JSON goes to `config/set`; read retained; delete clears retained; no file upload. |
| `test_offline_queue_credentials.py` | Queue enqueue/list/remove, corrupt files skipped, oldest first (also with entries of older tool versions); keyring save/read/clear. Regressions: same-second overwrite, CSV-only queue. |
| `test_workers.py` | What Push / Read / Send / Erase / Retry do per mode: SSH push then restart (not on failure), serial login passed through, MQTT failure queues the config, certificates uploaded first (failure stops), MQTT says certs must be placed manually, exceptions become failures, read/erase targets per mode, "Send Settings" doesn't restart, retry sends the JSON and removes only delivered entries, old CSV-only entries are kept with a message, MQTT certificate fields reach the transport. |
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
| `test_json` | `src/util/json.c` | String/int values, whitespace and newlines, missing keys leave the value unchanged, non-string rejected, truncation to the buffer, keys matched with their quotes (`"id"` ≠ `"device_id"`), `atoi` turns `"9600"`/`true` into 0, escapes decoded (`\"`, `\\`, `\/`, `\n`, `\uXXXX`), unterminated strings rejected, `json_value_end()` skips strings and nesting, `json_object_dup()` copies only that object (and skips the name used as a value), `*_from` = plain, `read_file`. |
| `test_payload` | `src/cloud/payload.c` | Empty message; every data type (`w`, `d`, `f` with 2 decimals, `b`); failed reads left out; no stray comma; a full buffer stops adding values and stays valid; NaN/Inf → `null`. |
| `test_store_forward` | `src/cloud/store_forward.c` | File named `<unix ms>_<seq>.txt`, unique within one second, sorting after files of older firmware; NULL / write failure; only `*.txt` are payloads; offline → nothing; oldest first, then deleted; unreadable file kept; list/remove failures reported; **kept when the broker doesn't confirm**; a round drains the backlog, limited to `SF_REPLAY_BATCH`, and stops at the first unconfirmed publish. |
| `test_ota_logic` | `src/cloud/ota_logic.c` | App and system commands parsed (sha256 only for app); other topics / empty payload ignored; missing URL; **https only**; URLs up to 4095 characters, longer rejected; `\/` decoded; app commands need a 64-hex sha256; exact status JSON; digest check; the app-update sequence step by step: happy path, no sha256 → nothing downloaded, download failure, SHA mismatch deletes the download, first install without a running binary, backup failure keeps the running binary, replace failure restores the backup. |
| `test_config_push` | `src/cloud/config_push.c` | A new config is saved and a reload requested; the same config (retained message on reconnect) is not rewritten; empty register list accepted; non-configs (empty, not JSON, no `device`/`registers`, trailing text) rejected without writing; NUL / oversized payloads; payload without NUL terminator; write failure keeps the old file; the exact ack JSON. |
| `test_mb_cmd` | `src/cloud/mb_cmd.c` | Modbus write commands (`MODBUS_WRITE_COMMANDS.md`): FC05/06/15/16 on configured registers reach the fake bus with the right slave, table, address and words; 32-bit registers only whole (FC16, high word first); unconfigured addresses, read-only tables, wrong table for the fc, another slave, an empty configuration → refused with the reason, **nothing written**; malformed JSON, missing/non-integer params (`"1"`, `1.5`, `1e3`), ranges, fc/method mismatch, count ≠ values, too many values, NUL/oversized/unterminated payloads; a retained command never executed; slave not confirming → error; bus locked during check and write and always released; exact, escaped response JSON. |
| `test_msg_queue` | `src/util/msg_queue.c` | FIFO order; payload copied; full queue drops the oldest; wrap-around; invalid arguments; items still delivered after shutdown; shutdown and push wake a blocked consumer (threads); destroy frees pending items (LeakSanitizer). |
| `test_data_read` | `src/fieldbus/data.c` (+ `settings.c`, `json.c`) with a fake fieldbus driver | uint16 reads; each table read from its own Modbus table; float32 and int32 **high word first**; signed int32; int32 above 2^24 exact; NaN/huge floats converted without undefined behaviour; slave switched per point; retries (`MAX_RETRIES`) then success; failure marks the point invalid; no driver → invalid; register and block writes. |

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

---

## 10. Release script tests (`tests/unit/release/`)

`test_make_release.py` covers `tools/release/make_release.py` (run by the `unit-tool` job):

* the version comes from `APP_VERSION_DEF` in `inc/settings.h`; the tag must equal
  `v<version>` (`v9.9.9`, `1.0.0`, `v1.0.0-rc1`, `release` fail; no tag is allowed for dry runs);
* packaging from a fake build directory: file names, the binary is executable and identical to
  `build/main`, the libraries tarball holds `lib/*.so*`, `SHA256SUMS` verifies, release notes
  carry version, commit, checksum and the OTA topic;
* `ota_command.json`: `sha256` is the binary's (64 hex, what the firmware requires), the URL
  comes from `OTA_URL` or is a placeholder **without `https://`**, which the firmware refuses
  (checked against `ota_logic.c`);
* wrong builds are refused before anything is written: missing binary or libraries, another
  `app_version`, a non-ARM binary, a binary that doesn't match `build_info.json`;
* publishing (HTTP calls recorded, nothing sent): every file uploaded to the Generic Package
  Registry under `stm32mp1-gateway/<version>`, one Release for the tag with a link per file;
  `--dry-run` or no tag never publishes.

---

## 11. Hardware-in-the-loop tests (`tests/hil/`)

The CI build, installed with the production installer on the CI DK2, against simulated
Modbus slaves and (optionally) AWS IoT. Rig and one-time setup: [`HIL_SETUP.md`](HIL_SETUP.md);
the job and the file-by-file table: [`CI_CD_GUIDE.md` §5.9](CI_CD_GUIDE.md#59-hil-tests--hardware-in-the-loop-on-the-ci-dk2).

### 11.1 How a session works

* `board` (session fixture, `conftest.py`): SSH to the board, back up `/etc/gateway` and the
  buffered payloads, install `gateway-<ver>.tar.gz` with `install.sh --no-migrate`; at the end
  save the session's journal to `reports/hil-journal.log` and restore the board.
* `gateway`: `apply_config(cfg)` writes `/etc/gateway/smart_rtu_config.json`, restarts the
  service and waits until the registers are loaded; `wait_stored()` returns new telemetry
  payload files (with no broker configured every cycle's payload is stored offline — the same
  JSON that is published, so the cloud contract is checked without AWS).
* `slaves` (`sim.py`): Modbus RTU slaves 1 and 2 on the RS485 adapter and a Modbus TCP slave on
  this PC, in one asyncio loop (pymodbus 3.6). `RTU_REGISTERS` in `conftest.py` lists every test
  register with the value the slave holds.
* `board.py`: `run()`, `put()`, `read()`, journal positions `mark()` / `since()` /
  `wait_log(mark, regex)` — use `wait_log` instead of sleeps.
* `aws_link.py`: `skip_without_network()`, `Observer` (subscribe, `wait_json()`, `publish()`;
  always closed; AWS by default, any TLS broker with `host=`/`port=`/certificates).
* `local_broker.py`: MQTT without AWS and without a firewall opening — mosquitto on this PC
  (127.0.0.1, client certificates from a per-run CA) and a **reverse port forward on the SSH
  connection**, so the gateway connects to `localhost:18883` on the board. `install_device_certs()`,
  `mqtt_section()`, `observer()`. Needs the `mosquitto` package (the job and `run_local.sh` install it).

### 11.2 Rules

* Test configs keep **Wi-Fi disabled** (`base_config()` does): the board's network is never touched.
* Never print a secret; `--tb=short` keeps argument values out of tracebacks.
* Undo everything you change on the board outside `/etc/gateway` and the payload storage.
* The board's BusyBox tools are limited (`find` has no `-delete`, no `ldd`): test commands
  with them before relying on GNU options.
* A regression found on the board gets a HIL test **and**, where the logic allows, a host test.

### 11.3 Found on the board while building the suite

| Problem | Fixed | Guarded by |
|---|---|---|
| Every payload was also POSTed to `https://httpbin.org/post` (public test service) | removed | `test_02_config_reload.py::test_no_third_party_http_post` |
| libmodbus frame debug always on (every request in the log) | off; `GATEWAY_MODBUS_DEBUG=1` to enable | — |
| `systemctl stop/restart` took 90 s and ended in SIGKILL (replay thread slept 60 s at a time; the drive logger's reader thread blocked forever on its pipe) | 1 s sleep steps; reader woken on stop; `TimeoutStopSec=20` | `test_99_no_crash.py::test_no_crash_in_journal` (SIGKILL is a crash) |
| Modbus TCP database in `/tmp` (lost at reboot) | `/var/lib/gateway/modbus_tcp.db` | `test_04_modbus_tcp.py::test_database_in_data_dir` |
| Offline storage path hard-coded to the legacy directory | `<data dir>/storage` | `test_05_store_forward.py` |
| A restart during the 30 s Wi-Fi wait at start-up ended in SIGKILL (the wait ignored the stop request) | the wait stops on shutdown | `test_99_no_crash.py` |
| Modbus TCP thread gave up for good when the slave was down at start-up; its argument was a pointer to a stack variable that had gone out of scope | retries every 5 s; argument `static` | `test_04_modbus_tcp.py` (needs the firewall opening, `HIL_SETUP.md` §3) |
| RS485 replies never reached the board: the image's device tree didn't mux PE9 as UART7 RTS (DE) nor enable RS-485 (a 157C board; the README change was for a 157F) | board DTB patched (`DEVICETREE_RS485.md`) | `test_03_modbus_rtu.py` (5 tests) |
