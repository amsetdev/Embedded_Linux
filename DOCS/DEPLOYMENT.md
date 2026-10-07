# Deployment — STM32MP157F-DK2 gateway

How the application is installed on a board, where everything lives, and how to
update, roll back and troubleshoot it. The same package and `deploy/install.sh` are
used by developers (`make install-board`), releases (`gateway-<ver>.tar.gz`) and the
hardware-in-the-loop CI (step 7), so CI tests exactly what production runs.

---

## 1. Layout on the board

| Path | What | Owner / mode |
|---|---|---|
| `/opt/gateway/bin/gateway` | the application | root 0755 |
| `/opt/gateway/bin/gateway.prev` | previous binary (for `install.sh --rollback`) | |
| `/opt/gateway/bin/gateway.bak` | binary before the last app OTA (written by OTA) | |
| `/opt/gateway/lib/` | bundled runtime libraries (libmodbus, libmosquitto, libcurl, OpenSSL, …) | |
| `/opt/gateway/VERSION` | installed version | |
| `/etc/gateway/smart_rtu_config.json` | configuration (written by the Smart RTU tool, or pushed over MQTT) | 0600 (Wi-Fi password) |
| `/etc/gateway/smart_rtu_config.csv` | CSV copy the tool writes next to it (not read by the app) | |
| `/etc/gateway/certs/` | `ca.crt`, `client.crt`, `private.key` for AWS IoT | dir 0700, files 0600 |
| `/var/lib/gateway/storage/` | offline buffer: one `<unix ms>_<seq>.txt` per unsent payload | |
| `/var/lib/gateway/modbus_tcp.db` | Modbus TCP readings (SQLite) | |
| `/var/lib/gateway/ota/` | app OTA downloads | |
| `/etc/systemd/system/gateway.service` | the service (enabled: starts at boot) | |
| journald | all application output: `journalctl -u gateway` | |

The service runs:
```
/opt/gateway/bin/gateway --config /etc/gateway/smart_rtu_config.json --data-dir /var/lib/gateway
```
Without options the binary uses `smart_rtu_config.json` and data in its working directory
(the legacy behaviour). `gateway --version` prints the version, `--help` the options.

### Why the libraries are bundled

The binary is cross-compiled on Ubuntu 22.04 (CI). It finds its libraries in
`/opt/gateway/lib` first through its RPATH (`$ORIGIN/../lib`), so it never needs
anything copied into the OS's `/usr/lib` (the old `make deploy-libs` did that and could
break OS tools). glibc and `libgcc_s` are not bundled: they come from the board's OS
(glibc 2.39 on OpenSTLinux scarthgap; the build needs ≥ 2.35). Programs the app starts
(`wpa_cli`, `swupdate`) keep using the OS libraries.

Long term, the cleanest setup is to build with the board's Yocto SDK (OpenSTLinux SDK)
against the image's own libraries; then `lib/` can be dropped from the package.

---

## 2. Install or update

**From a checkout (developer):**
```bash
make install-board BOARD=root@192.168.1.26          # SSH key, or: SSHPASS=… make install-board …
```
This builds `build/gateway-<version>.tar.gz` (`make package`), copies it to the board and
runs `install.sh` there.

**From a release:** see the release notes (Deploy → Releases):
```bash
scp gateway-<ver>.tar.gz root@<board>:/tmp/
ssh root@<board> 'mkdir -p /tmp/gw && tar -C /tmp/gw -xzf /tmp/gateway-<ver>.tar.gz && sh /tmp/gw/install.sh'
```

**What `install.sh` does** (`deploy/install.sh`; options `--no-migrate`, `--no-start`, `--rollback`):

1. stops `gateway.service` and any legacy instance started by hand from
   `/home/root/edb_c/linking` (two instances would poll the same RS485 bus);
2. creates the directories above; installs the binary atomically (old one kept as
   `gateway.prev`) and replaces `/opt/gateway/lib`;
3. **first install on a legacy board** (no `/etc/gateway/smart_rtu_config.json` yet):
   copies `smart_rtu_config.json/.csv` and `ca.crt`/`client.crt`/`private.key` from
   `/home/root/edb_c/linking`, rewrites the certificate paths in the config to
   `/etc/gateway/certs/…`, and **moves** unsent payloads from `…/linking/storage` to
   `/var/lib/gateway/storage` (they are replayed when MQTT connects). The legacy
   directory itself is left in place;
4. installs and enables `gateway.service`, starts it, and checks that it is running and
   that `gateway --version` matches the package.

An existing `/etc/gateway` configuration is never overwritten by an update.

**Rollback:** `sh install.sh --rollback` (from any unpacked package) puts
`gateway.prev` back and restarts.

---

## 3. Operating it

| Task | Command (on the board) |
|---|---|
| status | `systemctl status gateway` |
| logs, live | `journalctl -u gateway -f` |
| reload config + registers without a restart (SIGHUP) | `systemctl reload gateway` |
| restart | `systemctl restart gateway` |
| stop / disable at boot | `systemctl stop gateway` / `systemctl disable gateway` |
| version | `/opt/gateway/bin/gateway --version` |
| buffered payloads | `ls /var/lib/gateway/storage \| wc -l` |

The Smart RTU tool (SSH mode) writes `/etc/gateway/smart_rtu_config.json`/`.csv`, uploads
certificates to `/etc/gateway/certs/` and runs `systemctl restart gateway`; "log tail" reads
`journalctl -u gateway`. Over MQTT the config arrives on `amset/<device_id>/config/set`
and is applied with a reload (`CI_CD_GUIDE.md` §6.1).

---

## 4. Migration history

| Date | Board | What happened |
|---|---|---|
| 2026-10-07 | CI DK2 `192.168.1.26` | first `install.sh` run: legacy instance (`nohup ./main` in `/home/root/edb_c/linking`) stopped, config + certificates migrated, 2,930 buffered payloads moved to `/var/lib/gateway/storage`; backup of the legacy directory: `/home/root/legacy-backup-20261007.tar.gz` |
