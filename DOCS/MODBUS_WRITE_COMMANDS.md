# Modbus write commands over MQTT

The cloud writes registers of the Modbus **RTU** slaves through the gateway:

```
cloud ──MQTT──► devices/<device_id>/commands ──► gateway (RTU master) ──RS485──► slave
cloud ◄──MQTT── devices/<device_id>/commands/response ◄──┘
```

Message format from the `feat/mb_write` branch, unchanged. Code: `src/cloud/mb_cmd.c`
(`inc/mb_cmd.h`), wired in `src/cloud/mqtt.c`. Modbus TCP is read-only (not covered).

## 1. Commands

Publish **QoS 1, not retained** on `devices/<device_id>/commands`:

```json
{"requestId": "42", "method": "mb_write_single",
 "params": {"slave": 1, "fc": 6, "addr": 10, "value": 1234}}

{"requestId": "43", "method": "mb_write_multiple",
 "params": {"slave": 1, "fc": 16, "addr": 20, "count": 2, "values": [65534, 7616]}}
```

| Field | Values |
|---|---|
| `requestId` | optional string (up to 64 characters), echoed in the response |
| `method` | `mb_write_single` (fc 5 or 6) or `mb_write_multiple` (fc 15 or 16) |
| `slave` | 1..247 |
| `fc` | 5 = coil, 6 = one holding register, 15 = coils, 16 = holding registers |
| `addr` | 0..65535, the address as in `smart_rtu_config.json` |
| `value` | single: coil 0/1, register word 0..65535 |
| `count`, `values` | multiple: 1..123 values, `count` = length of `values`; coils 0/1, words 0..65535 |

Numbers must be JSON integers (`7`, not `"7"`, `7.0` or `7e0`). Values are raw register
words: a negative int16 is sent as its two's complement (-1 → 65535); an **int32 / float32**
register is written with fc 16 as two words, **high word first** (the order the gateway
reads), e.g. int32 -123456 = `0xFFFE1DC0` → `[65534, 7616]`, float32 -12.5 → `[49480, 0]`.

## 2. Response

Every command is answered on `devices/<device_id>/commands/response` (QoS 1):

```json
{"requestId":"42","method":"mb_write_single","status":"ok"}
{"requestId":"42","method":"mb_write_single","status":"error","detail":"slave 1 addr 11 is not a configured holding register"}
```

`requestId` / `method` are left out when the command had none. `ok` means the slave
confirmed the write.

## 3. Safety rule: only configured registers

A write goes on the bus only if **every register it writes is in the loaded
`smart_rtu_config.json`**, for that slave, in the table that matches the function code:

| fc | Allowed on | Refused examples (`detail`) |
|---|---|---|
| 5, 15 | configured **coils** | `slave 1 addr 70 is DISC, a discrete input, not a coil` |
| 6 | a configured 16-bit (`uint16`) **holding register** | `S1_HOLD_I32 is int32 (2 registers): write both words with fc 16` |
| 16 | configured holding registers covering every written word; 32-bit registers only whole | `S1_HOLD_F32 (addr 30..31) must be written as a whole` |

Input registers and discrete inputs are read-only. A command that fails a check, is
malformed (`missing params.fc`, `value 70000 out of range 0..65535`, …) or arrives
**retained** (`retained commands are not executed` — a retained command would be written
again on every reconnect) is answered with `"status":"error"` and **nothing is sent on the
bus**. To allow writing a register, add it to the configuration (tool → push config).

A slave that doesn't confirm (timeout, Modbus exception): `slave 1 did not confirm the
write (timeout or exception)`.

## 4. Timing and threads

The command runs in the MQTT thread. The bus is shared with the polling thread through a
lock taken per register read (`data_bus_lock()`), so a command waits for at most one read
(normally a few ms; up to ~3 s when a slave is not answering) and a configuration reload
never happens in the middle of a write. The written value appears in the telemetry after
the next poll cycle.

## 5. AWS IoT policy

The gateway's policy needs, besides telemetry, config and OTA topics
(`HIL_SETUP.md` §4):

```json
{"Effect": "Allow", "Action": "iot:Subscribe",
 "Resource": "arn:aws:iot:REGION:ACCOUNT:topicfilter/devices/DEVICE_ID/commands"},
{"Effect": "Allow", "Action": "iot:Receive",
 "Resource": "arn:aws:iot:REGION:ACCOUNT:topic/devices/DEVICE_ID/commands"},
{"Effect": "Allow", "Action": "iot:Publish",
 "Resource": "arn:aws:iot:REGION:ACCOUNT:topic/devices/DEVICE_ID/commands/response"}
```

Whoever may publish on `devices/<id>/commands` can write those registers: give
`iot:Publish` on it only to the backend that should.

## 6. Try it by hand

```bash
mosquitto_pub -h <endpoint> -p 8883 --cafile AmazonRootCA1.pem --cert backend.crt --key backend.key \
  -q 1 -t devices/<id>/commands \
  -m '{"requestId":"t1","method":"mb_write_single","params":{"slave":1,"fc":6,"addr":10,"value":1234}}'
# on the board
journalctl -u gateway -f | grep -E "MB_CMD|DATA\] (Block )?Write"
```

## 7. Tests

* `tests/host/test_mb_cmd.c` — format, every safety-rule case, malformed input, retained,
  bus error, response JSON, bus locked during check + write.
* `tests/hil/test_07_mb_write.py` — on the CI DK2 over the real RS485 bus: FC05/06/16 written
  into the simulated slaves, int32 read back in telemetry, refused writes put nothing on the
  bus, a retained command isn't replayed after a reconnect. Uses a local broker
  (`tests/hil/local_broker.py`), no AWS needed.

## 8. Differences from `feat/mb_write`

| `feat/mb_write` | Now |
|---|---|
| any slave / address / table could be written | only configured, writable registers (§3) |
| the register table of a write was never set (uninitialised `reg_type`) | follows the function code |
| response only with a `requestId`, not escaped | always, escaped JSON |
| numbers read with `atoi` (`"abc"` → 0, `1.5` → 1) | strict integers, ranges checked |
| retained commands executed on every reconnect | answered, not executed |
| writes raced with the polling thread on one libmodbus context | bus lock |
| topics configurable under `"commands"` in the config | fixed `devices/<id>/commands[/response]` (the config schema has no `commands` section) |
