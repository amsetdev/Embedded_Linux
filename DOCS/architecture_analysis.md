# Architecture Analysis: Current Issues & Scalable Standard

## Current Architecture Issues at Scale

### 1. Hardcoded Credentials in Source Code

`settings.h:52-60` and `mb_tcp.h:64-84` embed MQTT broker URLs, usernames, and passwords as `#define` constants. These get compiled into the binary. At scale with hundreds of gateways, you cannot rotate credentials without rebuilding and redeploying firmware to every device.

### 2. Single Slave ID — No Multi-Device Support

The RTU thread (`main.c:171-250`) polls all registers using a single `cfg.modbus_slave` ID. Real industrial installations have multiple slave devices on the same RS485 bus. The current design cannot address them — you'd need one gateway per slave, which is wasteful.

### 3. One-Register-at-a-Time RTU Reads

`mb_transaction()` in `modbus.c:390` always requests quantity=1 (`tx[4]=0x00, tx[5]=0x01`). For 2000 registers, that's 2000 separate RS485 transactions per cycle. Modbus supports reading up to 125 registers in a single FC03 request. At scale this creates unnecessary bus traffic and makes polling cycles extremely slow.

### 4. SQL Injection in Modbus TCP Storage

`mb_tcp.c:86-88` builds SQL with `snprintf` and string interpolation for the timestamp:

```c
snprintf(sql, ..., "INSERT ... VALUES ('%s', %d, %u);", ts, address, value);
```

While the timestamp comes from `strftime` (safe today), this pattern is fundamentally unsafe. At scale with more data sources, this becomes a liability. Parameterized queries (`sqlite3_prepare_v2` + `sqlite3_bind_*`) should be used.

### 5. File-Based Offline Storage Doesn't Scale

`storage.c` stores each failed MQTT message as an individual `.txt` file, then replays by scanning the entire directory, doing string comparison to find the oldest file, and processing one file per 60-second cycle. With thousands of queued messages:

- Filesystem fills up (no size cap or rotation)
- `readdir` + string compare for chronological ordering is O(n) per replay cycle
- One message per minute replay rate means a backlog of 1000 messages takes ~16 hours to drain

### 6. `volatile int` Instead of Proper Atomics/Signals

`running` (`main.c:57`) and `mqtt_connected` (`mqtt.c:15`) use `volatile int` for cross-thread communication. `volatile` does not guarantee atomicity or memory ordering on ARM. At scale with more threads and faster polling, this is a race condition source. Should use `stdatomic.h` or `sig_atomic_t` + proper barriers.

### 7. No Message Queue Between Threads

The RTU thread directly calls `mqtt_publish()` inline (`main.c:229`). If MQTT publish blocks or is slow (TLS handshake, network latency), the entire Modbus polling cycle stalls. There's no decoupling — the polling rate is bound to the network speed.

### 8. Static `payload[PAYLOAD_MAX]` in Thread

`main.c:175` uses a `static char payload[PAYLOAD_MAX]` inside the thread function. This works with one thread but cannot scale to multiple RTU buses or parallel polling without data corruption.

### 9. No Watchdog / Health Monitoring

If the Modbus RTU thread hangs (UART deadlock, `tcdrain` never returns on a broken cable), nothing detects it. No hardware watchdog integration, no thread health heartbeat, no automatic recovery. In a fleet of devices, a silently dead gateway is worse than a crashed one.

### 10. Modbus TCP Config is Hardcoded in `main.c`

```c
static mb_thread_arg_t mb_arg = {
    .slave_ip = "192.168.0.101",
    .slave_port = 0,
    .slave_id = 0
};
```

This (`main.c:282-286`) isn't loaded from the config file. Adding more TCP slaves requires code changes and recompilation.

### 11. No OTA Update Mechanism

At scale, you cannot SSH into hundreds of gateways to deploy new firmware. There's no mechanism for over-the-air updates, version reporting, or rollback.

### 12. No Device Identity / Fleet Management

Every gateway uses the same MQTT topic `modbus/data` (`mb_tcp.h:74`). With multiple gateways publishing to the same topic, the cloud cannot distinguish which device sent which data. There's no device provisioning, certificate-per-device, or unique topic hierarchy.

### 13. Duplicate Configuration

MQTT broker, credentials, and topic are defined in both `settings.h` (for RTU path) and `mb_tcp.h` (for TCP path). Two sources of truth for the same configuration.

### 14. No Backpressure on MQTT Publish

`mqtt_publish()` in `mqtt.c:177` fires and forgets (QoS 1, no delivery confirmation check). If the broker is overwhelmed or the network is saturated, messages are silently lost or queued without bound in mosquitto's internal buffers.

---

## Standard Scalable Architecture

For a production IoT gateway at fleet scale (100s-1000s of devices), the standard architecture looks like this:

```
┌─────────────────────────────────────────────────────────────────┐
│                        GATEWAY DEVICE                           │
│                                                                 │
│  ┌──────────┐    ┌──────────────┐    ┌───────────────────────┐  │
│  │ Modbus   │    │              │    │    Message Broker      │  │
│  │ RTU Bus  │───>│  Fieldbus    │───>│   (ring buffer or     │  │
│  │ (multi-  │    │  Abstraction │    │    SQLite WAL queue)   │  │
│  │  slave)  │    │  Layer       │    │                        │  │
│  ├──────────┤    │              │    │  ┌───────┐ ┌────────┐ │  │
│  │ Modbus   │───>│  - RTU       │    │  │ MQTT  │ │ HTTP   │ │  │
│  │ TCP      │    │  - TCP       │    │  │ Pub   │ │ Upload │ │  │
│  │ (multi-  │    │  - Future:   │    │  └───┬───┘ └───┬────┘ │  │
│  │  slave)  │    │    EtherCAT  │    │      │         │      │  │
│  └──────────┘    │    OPC-UA    │    └──────┼─────────┼──────┘  │
│                  └──────────────┘           │         │         │
│  ┌──────────────────────────────┐           │         │         │
│  │   Device Manager             │           │         │         │
│  │  - Config (JSON/CBOR)        │           │         │         │
│  │  - OTA Update Agent          │           │         │         │
│  │  - Health/Watchdog           │           │         │         │
│  │  - Certificate Store         │           │         │         │
│  │  - Unique Device Identity    │           │         │         │
│  └──────────────────────────────┘           │         │         │
│                                             │         │         │
│  ┌──────────┐  ┌────────────┐               │         │         │
│  │ Display  │  │ Local API  │               │         │         │
│  │ (DRM/FB) │  │ (REST/WS)  │               │         │         │
│  └──────────┘  └────────────┘               │         │         │
└─────────────────────────────────────────────┼─────────┼─────────┘
                                              │         │
                               ┌──────────────▼─────────▼──────┐
                               │         CLOUD                  │
                               │  ┌─────────────────────────┐   │
                               │  │ MQTT Broker (EMQX/      │   │
                               │  │ HiveMQ) with per-device  │   │
                               │  │ ACLs & topics            │   │
                               │  └─────────────────────────┘   │
                               │  ┌─────────────────────────┐   │
                               │  │ Time-series DB           │   │
                               │  │ (InfluxDB/TimescaleDB)   │   │
                               │  └─────────────────────────┘   │
                               │  ┌─────────────────────────┐   │
                               │  │ Device Management        │   │
                               │  │ (provisioning, OTA,      │   │
                               │  │  monitoring, config push)│   │
                               │  └─────────────────────────┘   │
                               └────────────────────────────────┘
```

### Key Differences from Current Design

| Aspect | Current | Scalable Standard |
|---|---|---|
| **Bus access** | 1 slave, 1 register per request | Multi-slave, block reads (125 regs/request) |
| **Thread coupling** | Poll -> build -> publish inline | Poll -> queue -> publish (decoupled via message queue) |
| **Offline storage** | Flat files, 1/min replay | SQLite WAL-mode queue or ring buffer with batch replay |
| **Credentials** | `#define` in headers | Per-device X.509 certificates or secure element |
| **Config** | Partially from JSON, partially hardcoded | All from config, cloud-pushable |
| **Identity** | Single shared topic | `devices/{device_id}/telemetry` topic hierarchy |
| **Updates** | SSH + manual copy | OTA agent (e.g., SWUpdate, Mender, hawkBit) |
| **Monitoring** | `printf` to stdout | Structured logging + hardware watchdog + heartbeat MQTT topic |
| **Protocol abstraction** | Direct libmodbus calls in business logic | Fieldbus abstraction layer (add protocols without changing core) |
| **SQL** | String concatenation | Prepared statements |
| **Thread safety** | `volatile int` | `_Atomic` or `pthread` condition variables |
| **Backpressure** | None | Bounded queue with drop-oldest or block policy |

---

## Pros and Cons Summary

### Current Architecture

**Pros:**

- Simple, readable, easy to understand for a single-device deployment
- Low memory footprint — no complex frameworks
- Direct hardware control with minimal abstraction overhead
- Works reliably for a single gateway with one RS485 bus and one TCP slave
- Fast build, small binary, minimal dependencies
- Graceful degradation (display failure doesn't kill the app)

**Cons:**

- Cannot manage multiple Modbus slaves on the same bus
- Polling throughput bottleneck (1 register per transaction)
- Tight coupling between data acquisition and cloud publishing
- No fleet management, OTA, or remote configuration
- Credentials in source code
- Offline replay is O(n) and drains at 1 msg/min
- No watchdog or health monitoring
- Race conditions with `volatile` instead of atomics

### Scalable Architecture

**Pros:**

- Multi-slave, multi-protocol support out of the box
- Decoupled pipeline: polling rate independent of network speed
- Per-device identity enables fleet-wide management
- OTA updates eliminate manual deployment
- Bounded queues prevent memory exhaustion
- Structured logging enables remote diagnostics
- Protocol abstraction layer makes adding OPC-UA/EtherCAT straightforward

**Cons:**

- Significantly more code and complexity
- Higher memory and CPU usage (queues, threads, TLS per-device certs)
- Longer development cycle and more testing surface
- Requires cloud infrastructure (device registry, OTA server, certificate authority)
- Overkill for a single-device proof-of-concept
- Harder to debug on constrained hardware without proper tooling

---

## Recommended Migration Path

1. Fix critical issues first: block reads, SQL injection, credential extraction, atomic variables
2. Introduce message queue decoupling and multi-slave support

---

## Changes Implemented

The following architectural improvements have been applied to the codebase:

### 1. AWS IoT Core X.509 Mutual TLS (replaces HiveMQ username/password)

- `mqtt.c` now uses `mosquitto_tls_set()` with CA cert, device cert, and private key
- No username/password authentication — mTLS only
- TLS 1.2 minimum enforced for AWS IoT Core compatibility
- All certificate paths configurable via `smart_rtu_config.json`

### 2. Credentials Removed from Source Code

- Removed all hardcoded MQTT broker URLs, usernames, and passwords from `settings.h` and `mb_tcp.h`
- All MQTT configuration (broker endpoint, port, client ID, cert paths, topic) loaded from JSON config
- Default values are non-functional placeholders

### 3. Fieldbus Abstraction Layer

- New `fieldbus.h` defines a protocol-agnostic driver interface (vtable pattern)
- `fieldbus_rtu.c` — Modbus RTU driver wrapping existing `modbus.c`
- `fieldbus_tcp.c` — Modbus TCP driver wrapping libmodbus
- `data.c` reads registers through the driver vtable instead of calling `mb_transaction()` directly
- Adding a new protocol requires only implementing 4 functions and one new source file

### 4. Configuration Hot-Reload (SIGHUP)

- `SIGHUP` handler sets a flag checked in the main loop
- On SIGHUP: re-reads `smart_rtu_config.json`, disconnects MQTT, reconnects with new config
- Enables credential rotation without rebuilding or restarting the binary
- Usage: `kill -SIGHUP $(pidof main)`

### 5. Modbus TCP Config Moved to JSON

- Removed hardcoded IP `192.168.0.101` from `main.c`
- TCP slave IP, port, and slave ID loaded from `modbus_tcp` section in JSON config
- TCP thread only starts when `modbus_tcp.enable = 1`

### 6. New JSON Config Structure

```json
{
    "device": { ... },
    "wifi": { ... },
    "mqtt": {
        "broker": "<account>-ats.iot.<region>.amazonaws.com",
        "port": 8883,
        "client_id": "AMSET-001",
        "ca_cert": "/etc/ssl/certs/AmazonRootCA1.pem",
        "device_cert": "/etc/ssl/certs/device-certificate.pem.crt",
        "private_key": "/etc/ssl/private/device-private.pem.key",
        "topic": "smartrtu/AMSET-001/data"
    },
    "modbus_tcp": {
        "enable": 1,
        "ip": "192.168.0.101",
        "port": 502,
        "slave_id": 1
    },
    "registers": [ ... ]
}
```
### 7. Dual OTA Update Support (Application + System)

- New `ota.c` / `ota.h` module provides over-the-air update support via MQTT
- **Application OTA**: downloads new `main` binary from S3 pre-signed URL, verifies SHA256, backs up current binary, replaces, and restarts via `execv()`
- **System OTA**: downloads `.swu` image, invokes `swupdate -i` CLI to apply full-system update, reboots
- OTA commands received on `devices/{device_id}/ota/app` and `devices/{device_id}/ota/system` topics
- Status reports published to `devices/{device_id}/ota/status` with progress states (STARTED, DOWNLOADING, VERIFYING, APPLYING, SUCCEEDED, FAILED)
- OTA thread uses `pthread_cond_timedwait` (no polling) — responds instantly to commands, checks shutdown flag every 2s
- Concurrent update requests are rejected while one is in progress
- Backup/restore on failure: current binary copied to `.bak` before overwrite, restored if update fails
- All OTA settings configurable via `ota` section in `smart_rtu_config.json`
- Gated by `ota.enable` config flag — no overhead when disabled

### 8. All Modbus Register Types in Config Parser

- `parse_registers()` in `data.c` now reads `"type"` and `"data_type"` fields from each register entry in `smart_rtu_config.json`
- **Register types**: `"holding"` (FC 0x03, default), `"input"` (FC 0x04), `"coil"` (FC 0x01), `"discrete"` (FC 0x02)
- **Data types**: `"uint16"`/`"word"` (16-bit integer, default), `"float"`/`"float32"` (32-bit, reads 2 consecutive registers), `"bool"` (0/1 for coils and discrete inputs)
- Float registers use `read_block()` to fetch 2 registers, combined in big-endian word order (high word first) into an IEEE 754 float stored in `ModbusPoint.float_value`
- MQTT payload formatting is type-aware: bools output `true`/`false`, floats output `%.2f`, integers output `%d`
- Fully backward compatible — existing configs without `"type"` or `"data_type"` fields default to holding register with uint16 data type

```json
{
  "registers": [
    { "label": "Temperature", "address": 0, "type": "input", "data_type": "float" },
    { "label": "Pressure",    "address": 2, "type": "holding", "data_type": "uint16" },
    { "label": "Pump_Run",    "address": 0, "type": "coil", "data_type": "bool" },
    { "label": "Door_Open",   "address": 5, "type": "discrete", "data_type": "bool" },
    { "label": "Speed",       "address": 10 }
  ]
}
```

3. Add OTA and fleet management when deploying more than a handful of devices
