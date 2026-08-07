# OTA Service (standalone daemon)

Runs as its own process/systemd unit, separate from `gateway.service`,
so it can update or restart the gateway app without taking itself down.

## Build

```bash
# Cross-compile for the STM32MP1 target (arm-linux-gnueabihf toolchain)
make

# Native build on your dev machine, for quick testing of the logic
make HOST=1
```

Requires `libmosquitto-dev`, `libcurl4-openssl-dev`, `libssl-dev` (or their
equivalents in your Yocto/Buildroot SDK sysroot).

## Deploy on the board

```
/opt/ota/ota_service          <- the binary (WorkingDirectory in the unit)
/opt/ota/ota_config.json
/opt/ota/version.txt          <- currently installed firmware version
/opt/ota/update/              <- created automatically (downloads land here)
/opt/ota/backup/              <- created automatically (rollback backups)
/etc/systemd/system/ota-service.service
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ota-service.service
sudo systemctl status ota-service.service
```

## Runtime flow

1. `ota_service` connects to ThingsBoard over MQTT with its own client/token
   and subscribes to `v1/devices/me/rpc/request/+`.
2. A `fw_update` RPC arrives → the service ACKs immediately
   (`{"status":"started"}`) so the RPC call doesn't time out, then hands
   the work to a dedicated worker thread.
3. Worker thread: download `latest.json` → compare versions → download
   package + checksum → verify SHA256 → back up the current install →
   install → restart `gateway.service` (or reboot the board, if
   `reboot_after_update` is set) → health-check → roll back on failure.
4. Progress/state are published continually on `gateway/ota/status`,
   `gateway/ota/progress`, `gateway/ota/result` so ThingsBoard dashboards
   can track the update live.

See the comment block at the top of `ota/ota.h` for the full state diagram.
