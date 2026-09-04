# Firmware changes needed for the new single combined config file

The tool used to write three separate files to the board
(`registers.csv`, `wifi_config.json`, `device_config.json`). That has
been replaced with **one combined file**, written in two formats so the
firmware can read whichever is easier — both are always sent together
and always contain the exact same information:

```
/home/root/edb_c/linking/smart_rtu_config.csv
/home/root/edb_c/linking/smart_rtu_config.json
```

Both contain everything: Wi-Fi, Device Settings, Modbus Map Sizing, and
the register list, in one place. Nothing is split across files anymore.

## Combined file layout

**smart_rtu_config.json**
```json
{
  "device": {
    "device_id": "AMSET-001",
    "slave_id": 1,
    "baud": 9600,
    "parity": "None",
    "stop_bits": 1,
    "interval_sec": 30
  },
  "wifi": {
    "ssid": "PlantWifi",
    "password": "secret123"
  },
  "modbus_sizing": {
    "coils": 0,
    "alerts": 0,
    "holding_integers": 6,
    "holding_decimals": 4,
    "holding_double_integers": 0,
    "input_integers": 0,
    "input_decimals": 0,
    "input_double_integers": 0
  },
  "registers": [
    { "label": "point_1", "address": 0 },
    { "label": "point_2", "address": 1 }
  ]
}
```

**smart_rtu_config.csv** — the same information, in one plain-text file
with `[SECTION]` markers so it's still readable in a text editor or
Excel:

```
[DEVICE]
device_id,AMSET-001
slave_id,1
baud,9600
parity,None
stop_bits,1
interval_sec,30

[WIFI]
ssid,PlantWifi
password,secret123

[MODBUS_SIZING]
coils,0
alerts,0
holding_integers,6
holding_decimals,4
holding_double_integers,0
input_integers,0
input_decimals,0
input_double_integers,0

[REGISTERS]
label,address
point_1,0
point_2,1
```

Register type (holding/input) and data width (word/float32/int32) are
**not** written per-row anymore. Work them out from position using the
`modbus_sizing` counts, in this fixed order: Holding Integers, Holding
Decimals, Holding Double Integers, then Input Integers, Input Decimals,
Input Double Integers. Example above: `holding_integers=6` and
`holding_decimals=4` means registers 1–6 in the list are 16-bit holding
words, and registers 7–10 are 32-bit holding floats.

## 1. Read the combined file instead of three separate ones

Whichever format is easier for your C code — pick one. The JSON is
simplest if you already have a JSON parser in the firmware (e.g.
cJSON); the CSV needs no library at all if you'd rather keep a small
hand-written line parser.

Update `data.c` / wherever `registers.csv` was previously read to
instead read `smart_rtu_config.csv` (or `.json`), and derive each
register's type/width from `modbus_sizing` + its position in the list,
using the fixed order described above.

## 2. Subscribe to the config topic (MQTT mode)

In `mqtt.c`, alongside your existing publish-only client setup, add a
subscription and message callback:

```c
#define CONFIG_SET_TOPIC_FMT "amset/%s/config/set"
#define CONFIG_ACK_TOPIC_FMT "amset/%s/config/ack"

static char device_id[64] = "AMSET-001";  // load from smart_rtu_config

void on_mqtt_message(const char *topic, const char *payload, int len)
{
    char expected_topic[96];
    snprintf(expected_topic, sizeof(expected_topic),
             CONFIG_SET_TOPIC_FMT, device_id);

    if (strcmp(topic, expected_topic) != 0) return;

    /* Atomic replace: write to .tmp then rename, so a mid-cycle read by
       the RTU thread never sees a half-written file */
    const char *tmp_path = "/home/root/edb_c/linking/smart_rtu_config.csv.tmp";
    const char *final_path = "/home/root/edb_c/linking/smart_rtu_config.csv";

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fwrite(payload, 1, len, f);
    fclose(f);
    rename(tmp_path, final_path);

    extern volatile int reload_requested;
    reload_requested = 1;
}
```

Register the subscription after `mqtt_init()`:
```c
char sub_topic[96];
snprintf(sub_topic, sizeof(sub_topic), CONFIG_SET_TOPIC_FMT, device_id);
mqtt_subscribe(sub_topic, 1 /* QoS */);
```
(Exact API depends on which MQTT client library you're using — adapt
`mqtt_subscribe`/callback registration to match your existing `mqtt.c`.)

The tool also publishes a JSON copy on `amset/<device_id>/config/set.json`
(retained, best-effort) in case you'd rather parse JSON than CSV — you
only need to subscribe to one of the two topics, not both.

## 3. Reload-in-place instead of process restart

In `main.c`, add the flag and check it once per main loop iteration —
**not** inside the Modbus thread, so a reload never interrupts a read
mid-transaction:

```c
volatile int reload_requested = 0;
```

In the main `while (running)` loop:
```c
if (reload_requested) {
    reload_requested = 0;
    printf("[ RELOAD ] New smart_rtu_config.csv received — reloading\n");
    parse_config();   // your updated single-file parser
    char ack_json[128];
    snprintf(ack_json, sizeof(ack_json),
             "{\"status\":\"ok\",\"points_loaded\":%d}", data_get_count());
    char ack_topic[96];
    snprintf(ack_topic, sizeof(ack_topic), CONFIG_ACK_TOPIC_FMT, device_id);
    mqtt_publish_to(ack_topic, ack_json);
}
```

This avoids the multi-second gap a full `pkill; ./main` restart causes
(during which RTU polling and MQTT telemetry both stop) — the reload
happens between poll cycles instead.

## 4. device_id in settings

Keep a `device_id` field somewhere the firmware reads at startup (it
can now simply come from the `device` section of
`smart_rtu_config.json`/`.csv` itself) so each physical unit has a
stable identity independent of its IP — this is what the tool's MQTT
topic (`amset/<device_id>/config/set`) keys off of.

## Testing without touching firmware yet

You can validate the tool's MQTT push path today using `mosquitto_sub`
on your PC to simulate the board:

```bash
mosquitto_sub -h <broker> -p 8883 --cafile <ca.pem> \
  -u <username> -P <password> \
  -t "amset/AMSET-001/config/set" -v
```

Push a config from the tool and confirm the combined CSV content
arrives on that topic. Then manually publish a fake ack to confirm the
tool's ack-wait logic completes:

```bash
mosquitto_pub -h <broker> -p 8883 --cafile <ca.pem> \
  -u <username> -P <password> \
  -t "amset/AMSET-001/config/ack" \
  -m '{"status":"ok","points_loaded":10}'
```
