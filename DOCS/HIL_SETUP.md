# HIL rig setup — CI DK2, runner, AWS IoT, RS485 adapter

Step-by-step, for doing it without help. What the tests do: `TESTS_GUIDE.md` §11,
the CI job: `CI_CD_GUIDE.md` §5.9.

```
 this PC (192.168.1.2)                                   CI DK2 (192.168.1.26)
 ┌───────────────────────────────────────────┐  LAN/SSH  ┌──────────────────────────────┐
 │ gitlab-runner "stm32-hil" (Docker, host    │──────────►│ gateway.service (/opt/gateway)│
 │   network) runs tests/hil:                 │           │                              │
 │   - SSH: install package, configs, journal │  Modbus   │                              │
 │   - Modbus TCP slave :5020  ◄──────────────┼───TCP─────│ mb_tcp thread                │
 │   - Modbus RTU slaves 1,2 on the CH340 ────┼──RS485────│ ttySTM2 (RS485)              │
 │   - AWS IoT observer ─────► AWS IoT ◄──────┼───MQTT────│ MQTT (test certificate)      │
 └───────────────────────────────────────────┘           └──────────────────────────────┘
```

---

## 1. SSH key for the board (once — done 2026-10-08)

The CI job logs in as root with its own key: `~/.config/embedded_linux/hil/id_ed25519`
(installed in the board's `/home/root/.ssh/authorized_keys`, comment `gitlab-hil@192.168.1.2`).

The board runs **Dropbear 2022.83**, which does not support the `from="…"` key option
(a key line with it is ignored), so the key cannot be limited to this PC: anyone who can
run pipelines in this project can use it to log in as root on the CI board. Give push
rights only to people you trust with that. To redo or rotate it:

```bash
mkdir -p ~/.config/embedded_linux/hil && chmod 700 ~/.config/embedded_linux/hil
ssh-keygen -t ed25519 -N "" -C "gitlab-hil@192.168.1.2" -f ~/.config/embedded_linux/hil/id_ed25519
# install it on the board, restricted to logins from this PC (asks the root password once)
cat ~/.config/embedded_linux/hil/id_ed25519.pub | \
  ssh root@192.168.1.26 'mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
# rotate: first remove the old line (comment gitlab-hil@192.168.1.2) from the board's authorized_keys
# check: must log in without a password
ssh -i ~/.config/embedded_linux/hil/id_ed25519 root@192.168.1.26 'gateway_version=$(/opt/gateway/bin/gateway --version); echo ok $gateway_version'
```

For local runs (`tests/hil/run_local.sh`) create `~/.config/embedded_linux/hil/hil.env`:
```
HIL_BOARD_HOST=192.168.1.26
```

---

## 2. GitLab CI/CD variables (once, Maintainer)

Settings → CI/CD → Variables → Add variable:

| Key | Type | Value | Protect | Note |
|---|---|---|---|---|
| `HIL_BOARD_HOST` | Variable | `192.168.1.26` | no | |
| `HIL_SSH_KEY` | **File** | the key **text** — the whole content of `~/.config/embedded_linux/hil/id_ed25519` (`-----BEGIN …` to `-----END …`), **not** the path | no | not Protected so the manual HIL button works on MRs. Anyone who can run pipelines can use it (Dropbear can't restrict it to this PC, §1): give push rights only to people you trust with root on the CI board |
| `HIL_SIM_HOST` | Variable | `192.168.1.2` | no | optional (default); this PC as the board sees it |

AWS variables: §4.

---

## 3. The runner `stm32-hil` (once)

A separate runner, so the board's runner can be paused or broken without touching the
others. It needs host networking (the board connects to the Modbus TCP slave the job
starts on this PC) and the RS485 adapter.

**Quick way** — create the runner in the web UI (step 1 below), then run the script; it does
steps 2–6 (backup of `config.toml`, register, take the CH340 away from `esp32-tester`, ufw
rule, restart, verify) and, with a personal access token (scope `api`), the CI variables of §2:
```bash
sudo tests/hil/setup_runner.sh glrt-XXXXXXXXXXXXXXXXXXXX [glpat-XXXXXXXXXXXXXXXXXXXX]
```
Running it again is harmless. The manual steps it performs:

1. GitLab → `root/Embedded_Linux` → Settings → CI/CD → Runners → **New project runner**:
   Tags `stm32-hil`; untick "Run untagged jobs"; description `stm32-hil` → Create → copy
   the token `glrt-…`.
2. Register it (replace the token):
   ```bash
   sudo gitlab-runner register --non-interactive \
     --url http://192.168.1.2 --token glrt-XXXXXXXXXXXXXXXXXXXX \
     --executor docker --docker-image python:3.11-slim --description stm32-hil
   ```
3. `sudo nano /etc/gitlab-runner/config.toml`, find the `[[runners]]` block with
   `name = "stm32-hil"`, and in **its** `[runners.docker]` section add/set:
   ```toml
   network_mode = "host"
   devices = ["/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0:/dev/ttyUSB0"]
   pull_policy = ["if-not-present"]
   ```
4. In the **`esp32-tester`** block, remove the CH340 entry from `devices` (§5), so the
   ESP32 HIL job can never open the adapter while it is wired to the DK2:
   ```toml
   devices = ["/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0:/dev/ttyUSB0"]
   ```
5. Apply and check:
   ```bash
   sudo gitlab-runner restart
   sudo gitlab-runner verify          # stm32-hil: is alive
   sudo gitlab-runner list
   ```
6. If Docker reports `port 5020 already in use`, something else listens on it: `sudo ss -ltnp | grep 5020`.
   If the board can't reach the TCP slave, allow it: `sudo ufw allow from 192.168.1.26 to any port 5020 proto tcp`
   (only when ufw is active: `sudo ufw status`).

`concurrent = 1` (global) still applies: HIL waits for other jobs on this PC. The job's
`resource_group: stm32-hil-board` also stops two pipelines from using the board at once.

---

## 4. AWS IoT test identities (once; the network tests skip until this exists)

Use test-only identities with strict policies — never the production certificate.
Replace `REGION`, `ACCOUNT` and, if you choose another name, `HIL-DK2` everywhere.

1. **Thing for the gateway**: AWS IoT Core → Manage → All devices → Things → Create
   things → single thing `HIL-DK2` → Auto-generate a new certificate → attach policy
   `hil-dk2-device` (create it from this JSON) → download the **device certificate**,
   **private key** and **Amazon Root CA 1**:
   ```json
   {"Version": "2012-10-17", "Statement": [
     {"Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:REGION:ACCOUNT:client/HIL-DK2"},
     {"Effect": "Allow", "Action": "iot:Publish", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topic/hil/HIL-DK2/telemetry",
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/ota/status",
       "arn:aws:iot:REGION:ACCOUNT:topic/amset/HIL-DK2/config/ack",
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/commands/response"]},
     {"Effect": "Allow", "Action": "iot:Subscribe", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/devices/HIL-DK2/ota/app",
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/devices/HIL-DK2/ota/system",
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/amset/HIL-DK2/config/set",
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/devices/HIL-DK2/commands"]},
     {"Effect": "Allow", "Action": "iot:Receive", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/ota/app",
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/ota/system",
       "arn:aws:iot:REGION:ACCOUNT:topic/amset/HIL-DK2/config/set",
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/commands"]}]}
   ```
   This is also what a **production** gateway's policy needs (with its own device ID and
   telemetry topic): the config topics are new since the bug-fix pass (`CI_CD_GUIDE.md` §6.1),
   the `commands` topics come with the Modbus write commands (`MODBUS_WRITE_COMMANDS.md`).
   The write-command HIL tests themselves use a local broker, not AWS.
2. **Observer certificate** (the tests' own MQTT client): Security → Certificates → Add
   certificate → Create certificate → activate → download cert + key → attach policy
   `hil-dk2-observer`:
   ```json
   {"Version": "2012-10-17", "Statement": [
     {"Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:REGION:ACCOUNT:client/HIL-DK2-observer"},
     {"Effect": "Allow", "Action": "iot:Subscribe", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/hil/HIL-DK2/telemetry",
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/devices/HIL-DK2/ota/status",
       "arn:aws:iot:REGION:ACCOUNT:topicfilter/amset/HIL-DK2/config/ack"]},
     {"Effect": "Allow", "Action": "iot:Receive", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topic/hil/HIL-DK2/telemetry",
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/ota/status",
       "arn:aws:iot:REGION:ACCOUNT:topic/amset/HIL-DK2/config/ack"]},
     {"Effect": "Allow", "Action": ["iot:Publish", "iot:RetainPublish"], "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topic/amset/HIL-DK2/config/set"]},
     {"Effect": "Allow", "Action": "iot:Publish", "Resource": [
       "arn:aws:iot:REGION:ACCOUNT:topic/devices/HIL-DK2/ota/app"]}]}
   ```
   The config is published **retained**, which needs `iot:RetainPublish`.
3. **Endpoint**: AWS IoT Core → Settings → Device data endpoint (`xxxx-ats.iot.REGION.amazonaws.com`).
4. **GitLab variables** (Settings → CI/CD → Variables, all **Protected**: they reach only
   `main`, tags and protected branches):

   | Key | Type | Value |
   |---|---|---|
   | `HIL_AWS_ENDPOINT` | Variable | the endpoint host name |
   | `HIL_DEVICE_ID` | Variable | `HIL-DK2` |
   | `HIL_AWS_CA` | **File** | Amazon Root CA 1 (PEM) |
   | `HIL_DEVICE_CERT` / `HIL_DEVICE_KEY` | **File** | the thing's certificate / private key |
   | `HIL_OBSERVER_CERT` / `HIL_OBSERVER_KEY` | **File** | the observer's certificate / private key |

5. **Locally**: add `HIL_AWS_ENDPOINT=…` and `HIL_DEVICE_ID=HIL-DK2` to `hil.env` and put the
   files into `~/.config/embedded_linux/hil/` as `aws-ca.pem`, `device.pem.crt`,
   `device.pem.key`, `observer.pem.crt`, `observer.pem.key` (`chmod 600`).

To run the AWS tests on a branch before merging, protect a branch pattern such as
`hil/*` (Settings → Repository → Protected branches) and push there; Protected variables
reach protected branches.

---

## 5. Moving the RS485 adapter between the DK2 and the ESP32 rig

There is one CH340 USB-RS485 adapter (`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`).
Exactly one runner may map it — the one whose board it is wired to.

**DK2 → ESP32 rig**
1. Make sure no HIL job is running (`docker ps | grep runner-`).
2. Rewire the adapter's A/B to the ESP32 board's RS485 terminals.
3. `sudo nano /etc/gitlab-runner/config.toml`:
   - `stm32-hil` block: remove the CH340 from `devices` (leave `devices = []`);
   - `esp32-tester` block: add it back as `/dev/ttyUSB1`:
     ```toml
     devices = ["/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0:/dev/ttyUSB0",
                "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0:/dev/ttyUSB1"]
     ```
4. `sudo gitlab-runner restart`.
5. Result: std_gw's Modbus HIL tests run again; this project's RTU tests **skip**
   ("HIL_RTU_PORT … not available"), everything else still runs.

**ESP32 rig → DK2**: the reverse — rewire to the DK2's RS485, remove the second entry from
`esp32-tester`, add `"/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0:/dev/ttyUSB0"` to
`stm32-hil`, restart the runner. Check with `ls -l /dev/serial/by-id/`.

A runner whose `devices` names an unplugged device fails every job at container start
("error gathering device information") — keep `devices` in sync with what is plugged in.

---

## 6. What a HIL run does to the board

1. backs up `/etc/gateway` and the payloads in `/var/lib/gateway/storage` to
   `/var/lib/gateway-hil-backup` (kept from an aborted earlier run if it exists);
2. installs the pipeline's `gateway-<ver>.tar.gz` with `install.sh --no-migrate`
   (so the board then runs the tested build);
3. applies test configurations (Wi-Fi **disabled**, so the board's network is never touched)
   and restarts / reloads the service;
4. restores `/etc/gateway` and the payloads, removes the backup directory, starts the service.

If a run is killed half-way, the next run restores the original backup. To restore by hand:
```bash
systemctl stop gateway
rm -rf /etc/gateway && cp -a /var/lib/gateway-hil-backup/etc-gateway /etc/gateway
find /var/lib/gateway/storage -maxdepth 1 -name '*.txt' -exec rm -f {} +
find /var/lib/gateway-hil-backup/storage -maxdepth 1 -name '*.txt' -exec mv {} /var/lib/gateway/storage/ \;
rm -rf /var/lib/gateway-hil-backup && systemctl start gateway
```
