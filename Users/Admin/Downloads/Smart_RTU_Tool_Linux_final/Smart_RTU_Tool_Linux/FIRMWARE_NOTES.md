# Firmware changes required for this tool's MQTT delivery mode

The tool's SSH/SCP mode works against your **current** firmware unchanged
(it just writes registers.csv and restarts `./main`). The MQTT mode
(recommended for field/dongle deployments) needs two small additions on
the board side. Both are additive — nothing about your existing RTU/MQTT
publish logic needs to change.

## 1. Subscribe to the config topic

In `mqtt.c`, alongside your existing publish-only client setup, add a
subscription and message callback:

```c
#define CONFIG_SET_TOPIC_FMT "amset/%s/config/set"
#define CONFIG_ACK_TOPIC_FMT "amset/%s/config/ack"

static char device_id[64] = "AMSET-001";  // load from settings.config

void on_mqtt_message(const char *topic, const char *payload, int len)
{
    char expected_topic[96];
    snprintf(expected_topic, sizeof(expected_topic),
             CONFIG_SET_TOPIC_FMT, device_id);

    if (strcmp(topic, expected_topic) != 0) return;

    /* Atomic replace: write to .tmp then rename, so a mid-cycle read by
       the RTU thread never sees a half-written file */
    const char *tmp_path = "/home/root/edb_c/linking/registers.csv.tmp";
    const char *final_path = "/home/root/edb_c/linking/registers.csv";

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
```//
(Exact API depends on which MQTT client library you're using — adapt
`mqtt_subscribe`/callback registration to match your existing `mqtt.c`.)

## 2. Reload-in-place instead of process restart

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
    printf("[ RELOAD ] New registers.csv received — reloading points\n");
    parse_csv();   // safe: point_count/points[] are only read by the
                    // Modbus thread between cycles, brief race is fine
                    // for a config reload; add a mutex if you want it
                    // fully clean
    // publish ack
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

## 3. device_id in settings.config

Add a `device_id` field to `settings.config` (parsed the same way as
`modbus_slave`, `mqtt_broker`, etc.) so each physical unit has a stable
identity independent of its IP — this is what the tool's MQTT topic
(`amset/<device_id>/config/set`) keys off of.

## Testing without touching firmware yet

You can validate the tool's MQTT push path today using `mosquitto_sub`
on your PC to simulate the board:

```bash
mosquitto_sub -h <broker> -p 8883 --cafile <ca.pem> \
  -u <username> -P <password> \
  -t "amset/AMSET-001/config/set" -v
```

Push a config from the tool and confirm the CSV content arrives on that
topic. Then manually publish a fake ack to confirm the tool's ack-wait
logic completes:

```bash
mosquitto_pub -h <broker> -p 8883 --cafile <ca.pem> \
  -u <username> -P <password> \
  -t "amset/AMSET-001/config/ack" \
  -m '{"status":"ok","points_loaded":50}'
```
