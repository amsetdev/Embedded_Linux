# CI/CD Guide — STM32MP157F-DK2 gateway (Embedded_Linux)

How the GitLab pipeline works, how to run every job on your own PC, how to read
a failure, and how to change things safely. This file describes what is
**actually implemented**; §10 lists what is still planned.

The setup mirrors the ESP32 project `std_gw` (same GitLab, same runner), adapted
to a cross-compiled Linux application on a Cortex-A7.

---

## 1. The pipeline at a glance

```
stage:  validate     static-analysis   build                      unit-test    hardware-test   release
        ────────     ───────────────   ─────                      ─────────    ─────────────   ───────
        (step 2)     (step 4)          build-firmware ──►         (steps 3/5)  (step 7)        (step 6)
                                       compiler-warnings
```

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
| `.gitlab/ci/build.yml` | `build-firmware`, `compiler-warnings` |
| `tools/ci/install_toolchain.sh` | **the** package list: armhf cross gcc + armhf libmodbus/mosquitto/sqlite3/ssl/curl/zlib. Used by CI and by the `Dockerfile` |
| `tools/ci/build_info.py` | writes `build/build_info.json` |
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
| compiler-warnings | `docker run --rm -v "$PWD":/project stm32mp1-build sh -c "make clean all > build.log 2>&1"` then `python3 tests/static/warnings_gate.py build.log` |

The `build/` directory created by Docker belongs to root. `sudo rm -rf build` or
build in a clone (`git clone -q . /tmp/el && cd /tmp/el`) if that bothers you.

**After changing `tools/ci/install_toolchain.sh`, rebuild your image**
(`docker build -t stm32mp1-build .`): the Dockerfile copies the script, so your
local image and CI stay identical only if you do.

---

## 4. Reading results in GitLab

* **Pipeline page → job → log**: full output; the last lines say why it failed.
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

---

## 6. Known issues

| Issue | Note |
|---|---|
| The AWS IoT key (`private.key`) and the board password (`.env`) are still in **git history**, and the project is **public** on this GitLab | removed from the tree (§9). Rotate both; consider making the project private |
| 9 compiler warnings | baseline (§5.2) |

---

## 7. Recipes for common changes

**New source file**: add it to `SRCS` in the `makefile` (and a new subdirectory
to `BUILD_SUBDIRS`). It is then built and warning-gated automatically.

**New library dependency**: `install_toolchain.sh` (`libfoo-dev:armhf`) +
`LDFLAGS` in the `makefile`; `collect-libs.sh` picks up the `.so` by itself.
Rebuild your local image.

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
| Job **pending** "no runner" | job tags don't match an online runner (`sudo gitlab-runner verify`) |
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
| 2 | validate | schema + rule checks for `smart_rtu_config.json` and the register CSV, negative configs, C enum ↔ tool table consistency |
| 3 | unit-test | `unit-tool`: pytest, headless PyQt5, fake SSH/serial/MQTT transports |
| 4 | static-analysis | `cppcheck` (baseline gate) and `doxygen` (0 warnings) |
| 5 | unit-test | `unit-firmware`: host C tests (Unity + ASan/UBSan) of libmodbus/mosquitto-free modules |
| 6 | release | tag `v<APP_VERSION_DEF>`, binary + libs + `SHA256SUMS` |
| 7 | hardware-test | dedicated DK2 over SSH (runner tag `stm32-hil`): Modbus TCP + RTU slaves, AWS IoT observer, SIGHUP reload, store-and-forward, crash detection |
