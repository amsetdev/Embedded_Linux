/**
 * @file fw_config_dump.c
 * @brief Host harness: runs the REAL firmware config parser on a config file
 *        and dumps what the firmware read as JSON.
 *
 * Compiled natively (gcc, ASan + UBSan) together with the unmodified
 * src/util/settings.c, src/util/json.c and src/fieldbus/data.c by
 * tests/validate/firmware_view.py. The validate stage compares this dump with
 * what the JSON file actually says, so any difference between the tool's
 * output and the firmware's strstr-based parser is caught without a board.
 *
 * Usage:
 *   fw_config_dump <out.json>               defaults only (settings_defaults())
 *   fw_config_dump <out.json> <config dir>  settings_load() + parse_registers()
 *                                           on <config dir>/smart_rtu_config.json
 *
 * The parser prints its own log to stdout, so the dump goes to <out.json>.
 */

#include "settings.h"
#include "data.h"

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

/** Defined in main.c in the firmware; read_all_points() references it. */
atomic_int running = 1;

/** Output file of the dump. */
static FILE *out;

/** 1 until the first field of the current JSON object has been written. */
static int first;

/**
 * @brief Write @p s as a JSON string literal (escaping quotes, backslashes, control chars).
 *
 * @param s NUL-terminated string.
 */
static void put_str(const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (*p == '"' || *p == '\\')
            fprintf(out, "\\%c", *p);
        else if (*p < 0x20)
            fprintf(out, "\\u%04x", *p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}

/** Separator before the next member of the current object. */
static void sep(void)
{
    if (!first)
        fputs(",\n", out);
    first = 0;
}

/**
 * @brief Dump one string field: "path": {"value": "...", "size": N}.
 *
 * @param path JSON path in the config file, e.g. "device.device_id".
 * @param v    Firmware buffer.
 * @param size sizeof() the firmware buffer.
 */
static void field_str(const char *path, const char *v, size_t size)
{
    sep();
    fprintf(out, "  \"%s\": {\"value\": ", path);
    put_str(v);
    fprintf(out, ", \"size\": %zu}", size);
}

/**
 * @brief Dump one int field: "path": {"value": N}.
 *
 * @param path JSON path in the config file.
 * @param v    Firmware value.
 */
static void field_int(const char *path, int v)
{
    sep();
    fprintf(out, "  \"%s\": {\"value\": %d}", path, v);
}

/** String member of AppSettings. */
#define STR(path, member) field_str(path, cfg.member, sizeof(cfg.member))
/** Int member of AppSettings. */
#define INT(path, member) field_int(path, cfg.member)

/**
 * @brief Dump every AppSettings field under the config-file key it is read from.
 *
 * Keep in sync with settings_load() (src/util/settings.c).
 */
static void dump_settings(void)
{
    STR("device.device_id", device_id);
    INT("device.slave_id", modbus_slave);
    INT("device.baud", modbus_baud);
    INT("device.interval_sec", interval);
    STR("device.parity", modbus_parity);
    INT("device.stop_bits", modbus_stop_bits);

    STR("wifi.ssid", wifi_ssid);
    STR("wifi.password", wifi_password);
    STR("wifi.country", wifi_country);
    INT("wifi.enable", wifi_enable);

    STR("mqtt.broker", mqtt_broker);
    INT("mqtt.port", mqtt_port);
    STR("mqtt.client_id", mqtt_client_id);
    STR("mqtt.ca_cert", mqtt_ca_cert);
    STR("mqtt.device_cert", mqtt_device_cert);
    STR("mqtt.private_key", mqtt_private_key);
    STR("mqtt.topic", mqtt_topic);

    INT("modbus_tcp.enable", modbus_tcp_enable);
    STR("modbus_tcp.ip", modbus_tcp_ip);
    INT("modbus_tcp.port", modbus_tcp_port);
    INT("modbus_tcp.slave_id", modbus_tcp_slave_id);

    INT("ota.enable", ota_enable);
    STR("ota.app_topic", ota_app_topic);
    STR("ota.system_topic", ota_system_topic);
    STR("ota.status_topic", ota_status_topic);
    STR("ota.download_dir", ota_download_dir);
    STR("ota.app_version", app_version);
    STR("ota.app_binary_path", app_binary_path);

    INT("watchdog.enable", watchdog_enable);
    INT("watchdog.timeout", watchdog_timeout);
}

/**
 * @brief Dump the register list parsed by parse_registers().
 */
static void dump_points(void)
{
    const ModbusPoint *pts = data_get_points();
    int n = data_get_count();

    fputs("[", out);
    for (int i = 0; i < n; i++)
    {
        fputs(i ? ",\n  {" : "\n  {", out);
        fputs("\"label\": ", out);
        put_str(pts[i].label);
        fprintf(out, ", \"address\": %d, \"reg_type\": %d, \"data_type\": \"%c\", \"slave_id\": %d}",
                pts[i].address, (int)pts[i].reg_type, pts[i].data_type, pts[i].slave_id);
    }
    fputs("]", out);
}

/**
 * @brief Entry point: see the file comment.
 *
 * @param argc Argument count.
 * @param argv Output path, optional config directory.
 * @return 0 on success, 2 on usage or I/O error.
 */
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: %s <out.json> [config dir]\n", argv[0]);
        return 2;
    }
    out = fopen(argv[1], "w");
    if (!out)
    {
        perror(argv[1]);
        return 2;
    }

    int load_rc = 0;
    int reg_rc = 0;

    if (argc == 3)
    {
        if (chdir(argv[2]) != 0)
        {
            perror(argv[2]);
            return 2;
        }
        load_rc = settings_load();
        reg_rc = parse_registers();
    }
    else
    {
        settings_defaults();
    }

    fprintf(out, "{\"settings_load\": %d, \"parse_registers\": %d, \"max_points\": %d, "
                 "\"label_size\": %d,\n\"fields\": {\n",
            load_rc, reg_rc, MAX_POINTS, LABEL_MAX);
    first = 1;
    dump_settings();
    fputs("\n},\n\"registers\": ", out);
    dump_points();
    fputs("\n}\n", out);
    fclose(out);
    return 0;
}
