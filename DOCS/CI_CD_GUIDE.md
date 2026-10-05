# CI/CD Guide — STM32MP157F-DK2 gateway (Embedded_Linux)

How the GitLab pipeline works, how to run every job on your own PC, how to read
a failure, and how to change things safely. This file describes what is
**actually implemented**; §10 lists what is still planned.

**What each test checks and how to write one: [`TESTS_GUIDE.md`](TESTS_GUIDE.md).**

The setup mirrors the ESP32 project `std_gw` (same GitLab, same runner), adapted
to a cross-compiled Linux application on a Cortex-A7.

---

## 1. The pipeline at a glance

```
stage:  validate           static-analysis   build                      unit-test    hardware-test   release
        ────────           ───────────────   ─────                      ─────────    ─────────────   ───────
        validate-configs   (step 4)          build-firmware ──►         (steps 3/5)  (step 7)        (step 6)
                                             compiler-warnings
```

* `validate-configs` (`needs: []`) starts immediately and takes seconds.
  `build-firmware` waits for the `validate` stage, so a bad config stops the build.
* `build-firmware` cross-compiles `build/main` for `arm-linux-gnueabihf` and
  collects the runtime libraries into `build/lib/`.
* `compiler-warnings` reads the build log and fails on any compiler warning that
  is not in the baseline (§5.2).
* Stages without jobs yet are skipped by GitLab.

### When a pipeline runs (`workflow:` in `.gitlab-ci.yml`)

| Event | Pipeline |
|---|---|
| Push to a branch with an open MR | one **MR pipeline** (no duplicate branch pipeline) |
| Push to a branch without an MR | branch pipeline |
| Push of a tag | tag pipeline |
| Build → Pipelines → **Run pipeline** | always allowed (`$CI_PIPELINE_SOURCE == "web"`) |

Recommended (Settings → Merge requests): **"Pipelines must succeed"** and protect
`main`, so an MR can only be merged with a green pipeline.

### Files

| Path | What |
|---|---|
| `.gitlab-ci.yml` | stages, workflow rules, global variables (`BUILD_IMAGE`), includes |
| `.gitlab/ci/validate.yml` | `validate-configs` |
| `.gitlab/ci/build.yml` | `build-firmware`, `compiler-warnings` |
| `tools/ci/install_toolchain.sh` | **the** package list: armhf cross gcc + armhf libmodbus/mosquitto/sqlite3/ssl/curl/zlib. Used by CI and by the `Dockerfile` |
| `tools/ci/build_info.py` | writes `build/build_info.json` |
| `tests/pytest.ini`, `tests/requirements.txt` | pytest config (markers, strict xfail) and test dependencies |
| `tests/schemas/smart_rtu_config.schema.json` | JSON schema of the config file |
| `tests/validate/` | config rules, the real-firmware-parser harness, contract tests, `validate_config.py` CLI |
| `tests/fixtures/configs/` | known-good test config (`full_config.json`) |
| `configs/` | real device configs, one per device (validated automatically; none yet) |
| `tests/static/warnings_gate.py` | compiler-warning gate |
| `tests/static/compiler-warnings-baseline.json` | warnings accepted when the gate was added (9) |
| `Dockerfile` | local build environment (same packages via `install_toolchain.sh`) |
| `makefile` | build (`all`), `-Wall -Wextra` in `WARNINGS`, deploy targets |
| `scripts/collect-libs.sh` | copies every non-libc runtime `.so` the binary needs into `build/lib/` |

---

## 2. Runner (one-time setup, already done)

* GitLab CE and the runner run on the same PC (`192.168.1.2`), project
  `root/Embedded_Linux`.
* Runner **`auto-tester`**: GitLab Runner 19.4.1, **Docker executor**, tags
  **`docker`, `esp-idf`**, `pull_policy = ["if-not-present"]`, `concurrent = 1`.
  It is shared with `std_gw`; jobs of this project use only the tag `docker`.
* Runner **`esp32-tester`** (tag `hardware`) is the **ESP32** HIL rig. It owns
  `/dev/ttyUSB0` (FTDI) and `/dev/ttyUSB1` (CH340 RS485). **No job of this project
  may use the tag `hardware` or those serial devices.** The DK2 gets its own
  runner (tag `stm32-hil`, step 7).
* Every job sets `tags:` (the default is `[docker]`). A job whose tags match no
  runner stays **pending forever**.
* With `concurrent = 1`, jobs of this project and of `std_gw` queue behind each other.

```bash
sudo journalctl -u gitlab-runner -f     # live runner log
docker ps                               # containers of the running job
docker system df                        # disk used by images/volumes
sudo gitlab-runner restart              # after editing /etc/gitlab-runner/config.toml
```

### Why the toolchain is installed in the job

The Docker executor can't build the repo's `Dockerfile`, and this GitLab has no
container registry (`/jwt/auth` → 404), so there is no place to push a prebuilt
image. `build-firmware` therefore starts from the stock `ubuntu:22.04` (the
Dockerfile's base) and runs `tools/ci/install_toolchain.sh`. The downloaded `.deb`
files are kept in the CI cache (`.cache/apt/`, key = hash of the script), so after
the first run only `apt-get update` goes to the network. Cost: ≈ 40–60 s per build.

If that ever becomes too slow, enable the GitLab container registry, build the
Dockerfile once, push it, and point `BUILD_IMAGE` at it (and delete the
`before_script`).

---

## 3. Run everything locally

Run from the repo root.

| Job | Local command |
|---|---|
| build (as you always did) | `docker build -t stm32mp1-build .` once, then `docker run --rm -v "$PWD":/project stm32mp1-build make clean all` |
| build exactly like CI | see §5.1 |
| validate-configs | `docker run --rm -v "$PWD":/src --tmpfs /src/build-host:exec -w /src python:3.11-slim sh -c "apt-get update -qq && apt-get install -y -qq gcc libc6-dev >/dev/null && pip install -q -r tests/requirements.txt && python -m pytest -c tests/pytest.ini tests/validate -p no:cacheprovider"` |
| one config only | same, with `python tests/validate/validate_config.py configs/<file>.json` as the last command |
| compiler-warnings | `docker run --rm -v "$PWD":/project stm32mp1-build sh -c "make clean all > build.log 2>&1"` then `python3 tests/static/warnings_gate.py build.log` |

`--tmpfs /src/build-host:exec` keeps the compiled parser harness out of your tree
(it is cached in `build-host/validate/` otherwise). On a PC with `gcc`, `pytest` and
`jsonschema` you can also run `python3 -m pytest -c tests/pytest.ini tests/validate` directly.

The `build/` directory created by Docker belongs to root. `sudo rm -rf build` or
build in a clone (`git clone -q . /tmp/el && cd /tmp/el`) if that bothers you.

**After changing `tools/ci/install_toolchain.sh`, rebuild your image**
(`docker build -t stm32mp1-build .`): the Dockerfile copies the script, so your
local image and CI stay identical only if you do.

---

## 4. Reading results in GitLab

* **Pipeline page → job → log**: full output; the last lines say why it failed.
* **Pipeline page → Tests tab**: per-test results (JUnit) of `validate-configs`.
  Known issues show as *skipped* (xfail).
* **Merge request → Code Quality widget**: new compiler warnings (and cppcheck
  findings, step 4).
* **Job → Browse / Download artifacts**: `build/main`, `build/lib/`,
  `build/build_info.json`, `build/build.log`.

---

## 5. Job by job

### 5.1 `build-firmware` — cross-compile for the DK2

**Does**

1. `bash tools/ci/install_toolchain.sh` in `ubuntu:22.04` (gcc 11.4
   `arm-linux-gnueabihf`, armhf `-dev` packages from `ports.ubuntu.com`).
2. `make clean all` → `build/main` + `build/lib/*.so*` (via `scripts/collect-libs.sh`).
   The full log goes to `build/build.log`.
3. Checks that `build/main` is a **32-bit ARM EABI5 hard-float** ELF
   (interpreter `ld-linux-armhf.so.3`).
4. `tools/ci/build_info.py build` → `build/build_info.json`.

**Artifacts** (4 weeks): `build/main`, `build/lib/`, `build/build_info.json`,
`build/build.log`.

`build_info.json` records: `app_version` (`APP_VERSION_DEF` in `inc/settings.h`),
commit, ref, pipeline, UTC time, compiler version, `file` output, size and
**SHA256 of the binary and of every library in `build/lib/`**. To check what a
board runs: `sha256sum /home/root/edb_c/linking/main` on the board and compare.

**Secrets**: none. The MQTT certificate, key and CA are **read at runtime from
the board** (`/home/root/edb_c/linking/{client.crt,private.key,ca.crt}`, paths in
`smart_rtu_config.json`); nothing is compiled in. Unlike `std_gw`, CI never needs
the device identity to build.

**Built sources**: the 18 files in `SRCS` in the `makefile`.
`src/fieldbus/modbus.c` and `src/control_logic.c` are **not built** and are
excluded from all checks (they stay in the repo for reference).

**Build exactly like CI, locally**
```bash
rm -rf /tmp/el && git clone -q . /tmp/el
docker run --rm -v /tmp/el:/p -w /p ubuntu:22.04 bash -c \
  "bash tools/ci/install_toolchain.sh && make clean all 2>&1 | tee build.log && python3 tools/ci/build_info.py build"
```

**Reading a failure**: scroll to the first `error:` line. Missing library at link
time (`cannot find -lfoo`) → add `libfoo-dev:armhf` to `install_toolchain.sh`
**and** rebuild your local image.

### 5.2 `compiler-warnings` — no new warnings

The makefile compiles with `-Wall -Wextra` (`WARNINGS` variable). The 9 warnings
that existed when the gate was added are in
`tests/static/compiler-warnings-baseline.json`:

| Where | Warning |
|---|---|
| `src/cloud/ota.c` `verify_sha256` | `SHA256_Init/Update/Final` deprecated since OpenSSL 3.0 (move to the `EVP_Digest*` API) |
| `src/cloud/ota.c` `ota_process_system` | return value of `system()` ignored |
| `src/main.c` `main` (×3), `src/fieldbus/mb_tcp.c` `mb_thread_func1`, `src/cloud/storage.c` `replay_worker` | `-Wstringop-truncation`: `strncpy(dst, src, sizeof dst - 1)` where `src` is as long as `dst` |

The job **fails only when a change adds a warning**. Warnings are matched by
file + function + flag + message, **not line number**, so editing code above an
old warning doesn't make it new. A second identical warning is new.

Warnings GCC reports inside a system header (`string_fortified.h`) are attributed
to the project line they were inlined from.

**Reading a failure**
```
compiler warnings: 10, baseline 9, new 1, fixed 0

NEW warnings (fix them; see DOCS/CI_CD_GUIDE.md 'compiler-warnings'):
  src/main.c:414: in file scope: warning: 'unused_ci_probe' defined but not used [-Wunused-variable]
```

**What to do**

* Fix the code. Don't add `-Wno-…` to the makefile or `#pragma GCC diagnostic` to
  silence a warning without a comment explaining why it is a false positive.
* You **fixed** baseline warnings: the job passes and lists them under "Fixed since
  baseline". Lock that in so they can't come back:
  ```bash
  docker run --rm -v "$PWD":/project stm32mp1-build sh -c "make clean all > build.log 2>&1"
  python3 tests/static/warnings_gate.py build.log --update-baseline
  git add tests/static/compiler-warnings-baseline.json
  ```
* Never run `--update-baseline` to make new warnings go away.
* The log goes to `./build.log` (ignored by git), not into `build/`: `make clean`
  would delete it.
* The log must come from a **clean** build: an incremental `make` only shows the
  warnings of the files it recompiled (the gate refuses a log with no compile lines).

### 5.3 `validate-configs` — device configs and the tool/firmware contract

**Why it exists.** The firmware reads `smart_rtu_config.json` with a small
hand-written parser (`src/util/json.c`) that finds every key with `strstr()` **from
the start of its section to the end of the file**. A key missing from one section is
silently read from a later one; a string stops at the first `"` (no unescaping); a
long string is cut to its buffer; `"9600"` or `true` become `0` (`atoi`). None of that
shows an error on the board.

**How it checks.** `tests/validate/firmware_view.py` compiles the **unmodified**
`src/util/settings.c`, `src/util/json.c` and `src/fieldbus/data.c` with
`tests/validate/fw_config_dump.c` for the PC (gcc, ASan + UBSan) and runs
`settings_load()` + `parse_registers()` on each config. Every field the firmware ends
up with is compared with what the file says (or the default from
`settings_defaults()` when the key is absent). A parser crash or sanitizer report on
any input fails the test too.

**Checks**

* Every `configs/**/*.json`, root `smart_rtu_config*.json` and
  `tests/fixtures/configs/*.json`:
  * schema `tests/schemas/smart_rtu_config.schema.json`: required sections and keys,
    types, ranges (baud list, parity `None/Even/Odd`, stop bits 1/2, slave 1–247,
    interval 1–3600 s, ports), `"` and `\` forbidden in strings, absolute cert paths,
    no unknown keys;
  * rules (`config_checks.py`): duplicate labels (they are the telemetry JSON keys),
    **overlapping registers** (`int32`/`float32` use Address and Address+1),
    32-bit types on coil/discrete tables, Address+1 beyond 65535, MQTT topic without
    `#`/`+`/leading `$`, AWS IoT topic limits (256 bytes, 7 `/`), Modbus TCP IP when
    enabled, worst-case **telemetry size** vs `PAYLOAD_MAX` (128 KB, `inc/mqtt.h`),
    at most `MAX_POINTS` (2000) registers;
  * the **real firmware parser** reads every value exactly as written; strings fit
    their `char[]` (label 63 bytes, device_id 63, broker 255, …; UTF-8 bytes count).
* Configs built by the tool's own `DeviceConfig` (`test_tool_contract.py`), its CSV
  export matching the JSON, and the type names (`test_type_consistency.py`).
* 41 negative cases and 10 "must be allowed" cases (`test_negative_configs.py`).

**Reading a failure**
```
configs/plant_a.json has 2 problem(s):
  firmware reads wifi.enable = 0, the file means 1 (the key is missing here, so the
    firmware's parser (src/util/json.c) took it from a later part of the file; write it explicitly)
  overlap: registers[7] 'P2_KWH' (float32, holding 6412-6413) reuses slave 1 holding
    register 6413 of registers[6] 'P1_KWH' (float32)
```
`registers[7]` is the 0-based index in the `registers` array. Fix the config (usually
in the tool or the Excel sheet), then re-export.

**Common changes**

| You want to… | Do |
|---|---|
| add / update a device config | put it in `configs/<device_id>.json`. Nothing else: the glob picks it up |
| check a config before sending it | `validate_config.py` (§3) |
| the firmware reads a new key | add it to `settings_load()` **and** to `dump_settings()` in `tests/validate/fw_config_dump.c`, to the schema, and to `tests/fixtures/configs/full_config.json` |
| the tool writes a new key | schema + fixture + a negative case |
| a firmware buffer size changes | nothing for the firmware check (the harness reports `sizeof`); update the schema `maxLength` |

---

## 6. Known issues

A test for a bug that isn't fixed yet stays **in the suite** as a strict xfail
(`@pytest.mark.xfail(strict=True, reason=…)`): it shows as *xfailed* and **fails once
the bug is fixed** (`XPASS(strict)`), as a reminder to turn it into a normal test.

| Issue | Note |
|---|---|
| The tool doesn't write `wifi.enable`; the firmware reads `modbus_tcp.enable` instead, so **Wi-Fi is disabled whenever Modbus TCP is** | `test_tool_contract.py::test_wifi_stays_enabled_when_modbus_tcp_disabled` |
| For a register on the device's default slave the tool omits `slave_id`; the firmware takes the **next register's** `slave_id` | `test_tool_contract.py::test_register_with_device_default_slave_polled_from_device_slave` |
| OTA topics are built from the default device_id before the config is read: **every gateway uses `devices/AMSET-001/ota/*`** (the tool writes no `ota` section) | `test_tool_contract.py::test_ota_topics_follow_configured_device_id` |
| The tool's own validation accepts labels > 63 bytes, labels with `"`, overlapping 32-bit registers (the validate rules reject them) | `test_tool_contract.py::test_tool_rejects_what_firmware_mishandles` (3 cases) |
| `inc/settings.h` contains a default Wi-Fi SSID and password (`WIFI_SSID_DEF`, `WIFI_PASSWORD_DEF`) | change it on that network, and make the defaults empty |
| The AWS IoT key (`private.key`) and the board password (`.env`) are still in **git history**, and the project is **public** on this GitLab | removed from the tree (§9). Rotate both; consider making the project private |
| 9 compiler warnings | baseline (§5.2) |

---

## 7. Recipes for common changes

**New source file**: add it to `SRCS` in the `makefile` (and a new subdirectory
to `BUILD_SUBDIRS`). It is then built and warning-gated automatically.

**New library dependency**: `install_toolchain.sh` (`libfoo-dev:armhf`) +
`LDFLAGS` in the `makefile`; `collect-libs.sh` picks up the `.so` by itself.
Rebuild your local image.

**New register type or data type**
1. Firmware: `RegType` (`inc/data.h`) / `reg_type_from_string()` or
   `data_type_from_string()` (`src/fieldbus/data.c`), and the read/payload code.
2. Tool: `VALID_REG_TYPES`/`VALID_DATA_TYPES` (`core/register_model.py`),
   `REG_TYPES`/`DATA_TYPES` (`ui/main_window.py`), `excel_import.py` synonyms.
3. Schema enums; `REG_TYPE_CODE`/`DATA_TYPE_CODE`/`REGISTER_WIDTH` in `config_checks.py`.
4. `test_type_consistency.py` fails until 1–3 agree. Add the type to `full_config.json`.

**New CI job**
* `tags: [docker]` comes from `default:`; never use `hardware` (ESP32 rig).
* Use `needs: []` if it doesn't need build artifacts, so it starts immediately.
* Once the release job exists (step 6): add the job to `release`'s `needs:`.
* Produce `artifacts: reports: junit:` (tests) or `codequality:` (findings) so
  results show in GitLab.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Job **pending** forever, runner idle | the runner isn't enabled for this project: `auto-tester` was registered as a std_gw project runner. Settings → CI/CD → Runners → *Other available runners* → **Enable for this project** (Admin → Runners → untick "Lock to current projects" if it's not offered). Done once on 2026-10-05 |
| Job **pending** "no runner" | job tags don't match an online runner (`sudo gitlab-runner verify`) |
| validate: `no C compiler found` | install `gcc libc6-dev` (the job's `before_script` does), or set `$CC` |
| validate: `firmware parser exited with 1` + an ASan/UBSan report | the firmware's parser has a memory bug on that input: fix `json.c`/`settings.c`/`data.c`, add the input as a negative case |
| validate: `firmware reads X = …, the file means …` | see §5.3; the hint says when a key was taken from a later section |
| No pipeline after push | workflow rules: a branch with an open MR only gets an MR pipeline (look in the MR) |
| `E: Unable to locate package …:armhf` / `404 … ports.ubuntu.com` | mirror problem or the package was renamed: retry; check `install_toolchain.sh` |
| `cannot find -lmodbus` locally on the host | build in Docker (§3); the host has no armhf libraries |
| `compiler-warnings`: `not a clean build log` | the log came from an incremental build; use `make clean all` |
| Pipeline waits a long time before starting | `concurrent = 1`: a `std_gw` job is running |
| `permission denied … docker.sock` locally | `sudo usermod -aG docker $USER`, then log out and back in |
| `git push` asks for a username | GitLab over HTTP: username + personal access token (`write_repository`) |

---

## 9. Secrets

### 9.1 What is secret and where it lives

| File | What it is | In git? | Where it lives |
|---|---|---|---|
| `client.crt`, `private.key` | AWS IoT device identity | **no** (removed, `.gitignore`: `*.crt`, `*.key`) | on each board in `/home/root/edb_c/linking/`; uploaded by the Smart RTU tool |
| `ca.crt` | AWS root CA (public) | no (`*.crt` rule) | on the board, same folder |
| `.env` | `SSHPASS` for `make deploy` / `deploy-libs` / `flash` (`sshpass -e`) | **no** | your PC only |

Backup of the removed files: `~/Embedded_Linux_secrets_backup/` (directory 0700,
files 0600). Your working copy still has them (ignored by git). **Other clones lose
them on `git pull`** (git deletes files that stop being tracked): copy them out
first, or restore with `git show <old commit>:private.key > private.key`.

CI's build needs none of them. The HIL stage (step 7) will use **test-only**
certificates for a dedicated AWS IoT thing with a strict policy, from
**Protected File variables**, never the production identity.

### 9.2 Rotate after the cleanup

The old key and password are in git history (and the project is public), so
treat them as leaked:

1. AWS IoT Core → Security → Certificates → **Create certificate**; attach the
   same policy/thing; activate. Install it on the boards (Smart RTU tool, or copy to
   `/home/root/edb_c/linking/` and send `SIGHUP`). Then deactivate and revoke the old one.
2. Change the board's root password (`passwd` on the board) and update your local `.env`.
3. Optional: GitLab → Settings → General → Visibility → **Private**.

Rewriting history (`git filter-repo` + force-push + everyone re-clones) is possible
but disruptive; rotation makes the leaked values useless.

---

## 10. Roadmap (not implemented yet)

| Step | Stage | Adds |
|---|---|---|
| 3 | unit-test | `unit-tool`: pytest, headless PyQt5, fake SSH/serial/MQTT transports |
| 4 | static-analysis | `cppcheck` (baseline gate) and `doxygen` (0 warnings) |
| 5 | unit-test | `unit-firmware`: host C tests (Unity + ASan/UBSan) of libmodbus/mosquitto-free modules |
| 6 | release | tag `v<APP_VERSION_DEF>`, binary + libs + `SHA256SUMS` |
| 7 | hardware-test | dedicated DK2 over SSH (runner tag `stm32-hil`): Modbus TCP + RTU slaves, AWS IoT observer, SIGHUP reload, store-and-forward, crash detection |
