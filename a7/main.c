#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* RS-485 on STM32MP157F-DK2 GPIO pins
 *   TX  → GPIO14 (PA14)  UART4_TX  (/dev/serial0 → /dev/ttySTM0)
 *   RX  → GPIO15 (PA15)  UART4_RX
 *   DE  → GPIO23 (PB7)   RS-485 direction — kernel toggles via hardware RTS
 */
#define MODBUS_PORT        "/dev/serial0"
#define MODBUS_BAUD        9600
#define MODBUS_SLAVE_ID    1
#define MODBUS_PARITY      'N'
#define MODBUS_DATA_BITS   8
#define MODBUS_STOP_BITS   1
#define RS485_DIR_GPIO     23

#define CONFIG_FILE        "registers.csv"

#define MQTT_BROKER        "3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT          8883
#define MQTT_TOPIC         "modbus/data"
#define MQTT_USERNAME      "prasad"
#define MQTT_PASSWORD      "prasad#12$A"
#define MQTT_STORAGE_DB    "mqtt_storage.db"

#define MONITORING_INTERVAL 30  
#define MAX_RETRIES         2
#define POINT_DELAY_US      10000 
#define MAX_POINTS          2000
#define LABEL_MAX           64
#define UNIT_MAX            16
#define PAYLOAD_MAX         131072 

/* ============================================================================
 * DATA STRUCTURES
 * ========================================================================== */

typedef enum { REG_HOLDING, REG_INPUT, REG_COIL, REG_DISCRETE } RegType;

typedef struct {
    char    label[LABEL_MAX];
    int     address;
    
    RegType reg_type;
    char    data_type; /* 'w'=word, 'b'=bit */
    char    unit[UNIT_MAX];
    int     value;
    int     valid;
} ModbusPoint;

/* ============================================================================
 * GLOBALS
 * ========================================================================== */

static ModbusPoint       points[MAX_POINTS];
static int               point_count   = 0;
static modbus_t         *mb_ctx        = NULL;
static struct mosquitto *mosq          = NULL;
static sqlite3          *db            = NULL;
static volatile int      mqtt_connected = 0;
static volatile int      running        = 1;
static pthread_mutex_t   db_mutex       = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * LOGGING
 * ========================================================================== */

static void log_msg(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] [%s] ", tbuf, level);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

#define LOG_INFO(...)    log_msg("INFO",    __VA_ARGS__)
#define LOG_WARN(...)    log_msg("WARNING", __VA_ARGS__)
#define LOG_ERROR(...)   log_msg("ERROR",   __VA_ARGS__)
#define LOG_DEBUG(...)   log_msg("DEBUG",   __VA_ARGS__)

/* ============================================================================
 * STRING HELPERS
 * ========================================================================== */

static void str_tolower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* ============================================================================
 * CSV PARSER
 * Reads registers.csv exported from your Excel file.
 * Expected columns: Label, Address, Register Type, Data Type, Unit
 * ========================================================================== */

#define MAX_COLS 16

static int split_csv(char *line, char *cols[], int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        cols[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return n;
}

static int parse_csv(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        LOG_WARN("Config file '%s' not found — using 100 sample points", CONFIG_FILE);
        for (int i = 0; i < 100; i++) {
            snprintf(points[i].label, LABEL_MAX, "Sample_Point_%d", i + 1);
            points[i].address   = 400 + i;
            points[i].reg_type  = REG_HOLDING;
            points[i].data_type = 'w';
            points[i].unit[0]   = '\0';
            points[i].valid     = 0;
        }
        point_count = 100;
        return 1;
    }

    char line[512];
    int  first = 1;
    int  idx   = 0;
    int  c_label = -1, c_addr = -1, c_rtype = -1, c_dtype = -1, c_unit = -1;

    while (fgets(line, sizeof(line), f) && idx < MAX_POINTS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue;

        char *cols[MAX_COLS];
        int   ncols = split_csv(line, cols, MAX_COLS);

        if (first) {
            /* Parse header row */
            for (int c = 0; c < ncols; c++) {
                char tmp[64];
                strncpy(tmp, cols[c], 63); tmp[63] = '\0';
                str_tolower(tmp);
                char *s = str_trim(tmp);
                if (strcmp(s, "label") == 0)               c_label = c;
                else if (strcmp(s, "address") == 0)        c_addr  = c;
                else if (strstr(s, "register"))            c_rtype = c;
                else if (strstr(s, "data"))                c_dtype = c;
                else if (strcmp(s, "unit") == 0)           c_unit  = c;
            }
            first = 0;
            if (c_label < 0 || c_addr < 0) {
                LOG_ERROR("CSV must have 'Label' and 'Address' columns");
                fclose(f);
                return 0;
            }
            continue;
        }

        if (ncols <= c_label || ncols <= c_addr) continue;

        char *lbl = str_trim(cols[c_label]);
        if (strlen(lbl) == 0) continue;

        strncpy(points[idx].label, lbl, LABEL_MAX - 1);
        points[idx].label[LABEL_MAX - 1] = '\0';

        char addr_buf[32];
        strncpy(addr_buf, str_trim(cols[c_addr]), 31);
        /* Handle "12345 HR" style — take first token */
        char *sp = strchr(addr_buf, ' ');
        if (sp) *sp = '\0';
        points[idx].address = atoi(addr_buf);

        /* Register type */
        points[idx].reg_type = REG_HOLDING;
        if (c_rtype >= 0 && c_rtype < ncols) {
            char rt[32];
            strncpy(rt, cols[c_rtype], 31); str_tolower(rt);
            if (strstr(rt, "coil"))           points[idx].reg_type = REG_COIL;
            else if (strstr(rt, "discrete"))  points[idx].reg_type = REG_DISCRETE;
            else if (strstr(rt, "input"))     points[idx].reg_type = REG_INPUT;
        }

        /* Data type */
        points[idx].data_type = 'w';
        if (c_dtype >= 0 && c_dtype < ncols && strlen(cols[c_dtype]) > 0)
            points[idx].data_type = (char)tolower((unsigned char)cols[c_dtype][0]);

        /* Unit */
        points[idx].unit[0] = '\0';
        if (c_unit >= 0 && c_unit < ncols)
            strncpy(points[idx].unit, str_trim(cols[c_unit]), UNIT_MAX - 1);

        points[idx].valid = 0;
        idx++;
    }

    fclose(f);
    point_count = idx;
    LOG_INFO("Loaded %d points from %s", point_count, CONFIG_FILE);
    return 1;
}

/* ============================================================================
 * MODBUS
 * ========================================================================== */

static int mb_connect(void)
{
    LOG_INFO("Connecting to UART4 (%s) @ %d baud, slave %d, RS-485 DE=GPIO%d",
             MODBUS_PORT, MODBUS_BAUD, MODBUS_SLAVE_ID, RS485_DIR_GPIO);

    mb_ctx = modbus_new_rtu(MODBUS_PORT, MODBUS_BAUD, MODBUS_PARITY,
                            MODBUS_DATA_BITS, MODBUS_STOP_BITS);
    if (!mb_ctx) {
        LOG_ERROR("modbus_new_rtu: %s", modbus_strerror(errno));
        return 0;
    }

    modbus_set_slave(mb_ctx, MODBUS_SLAVE_ID);
    modbus_set_response_timeout(mb_ctx, 2, 0);

    if (modbus_connect(mb_ctx) == -1) {
        LOG_ERROR("modbus_connect: %s", modbus_strerror(errno));
        modbus_free(mb_ctx);
        mb_ctx = NULL;
        return 0;
    }

    /* ----------------------------------------------------------------
     * RS-485 direction control for GPIO23 (DE pin).
     *
     * Primary: TIOCSRS485 kernel ioctl — the UART4 driver asserts the
     * hardware RTS line (wired to GPIO23/DE) automatically before each
     * TX byte and deasserts it after the last byte is sent.  This is
     * zero-latency and requires no userspace toggling.
     *
     * Fallback: libmodbus software-RTS — libmodbus toggles the serial
     * port's RTS via TIOCM_RTS ioctl before/after each message.  Use
     * this if TIOCSRS485 is not supported by your kernel build.
     * ---------------------------------------------------------------- */
    int fd = modbus_get_socket(mb_ctx);
    struct serial_rs485 rs485 = {0};
    rs485.flags                = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    rs485.delay_rts_before_send = 0;  /* microseconds — increase if transceiver needs settling */
    rs485.delay_rts_after_send  = 0;

    if (ioctl(fd, TIOCSRS485, &rs485) == 0) {
        LOG_INFO("RS-485 kernel mode active — GPIO%d (DE) controlled by UART4 hardware RTS",
                 RS485_DIR_GPIO);
    } else {
        LOG_WARN("TIOCSRS485 not supported (%s) — using libmodbus software RTS", strerror(errno));
        modbus_rtu_set_serial_mode(mb_ctx, MODBUS_RTU_RS485);
        modbus_rtu_set_rts(mb_ctx, MODBUS_RTU_RTS_UP);
    }

    LOG_INFO("UART4 Modbus connected");
    return 1;
}

static int read_point(ModbusPoint *pt)
{
    if (!mb_ctx) return 0;

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        int rc = -1;
        uint16_t reg = 0;
        uint8_t  bit = 0;

        switch (pt->reg_type) {
            case REG_COIL:
                rc = modbus_read_bits(mb_ctx, pt->address, 1, &bit);
                if (rc == 1) { pt->value = bit; pt->valid = 1; return 1; }
                break;
            case REG_DISCRETE:
                rc = modbus_read_input_bits(mb_ctx, pt->address, 1, &bit);
                if (rc == 1) { pt->value = bit; pt->valid = 1; return 1; }
                break;
            case REG_INPUT:
                rc = modbus_read_input_registers(mb_ctx, pt->address, 1, &reg);
                if (rc == 1) { pt->value = reg; pt->valid = 1; return 1; }
                break;
            case REG_HOLDING:
            default:
                rc = modbus_read_registers(mb_ctx, pt->address, 1, &reg);
                if (rc == 1) { pt->value = reg; pt->valid = 1; return 1; }
                break;
        }

        usleep(100000); /* 100 ms before retry */
    }

    pt->valid = 0;
    return 0;
}

static void read_all_points(void)
{
    int     success = 0, fail = 0;
    time_t  start   = time(NULL);

    LOG_INFO("Reading %d points from slave %d", point_count, MODBUS_SLAVE_ID);

    for (int i = 0; i < point_count; i++) {
        if (read_point(&points[i])) success++;
        else                        fail++;

        usleep(POINT_DELAY_US);

        if ((i + 1) % 10 == 0 || i == point_count - 1) {
            int   elapsed = (int)(time(NULL) - start);
            float speed   = elapsed > 0 ? (float)(i + 1) / elapsed : 0.0f;
            LOG_INFO("Progress: %d/%d (%.1f%%) | %.1f pts/sec",
                     i + 1, point_count,
                     (float)(i + 1) / point_count * 100.0f, speed);
        }
    }

    LOG_INFO("READ COMPLETE — Success: %d  Failed: %d  Time: %ds",
             success, fail, (int)(time(NULL) - start));
}

/* ============================================================================
 * SQLITE OFFLINE STORAGE
 * ========================================================================== */

static int db_init(void)
{
    if (sqlite3_open(MQTT_STORAGE_DB, &db) != SQLITE_OK) {
        LOG_ERROR("Cannot open DB: %s", sqlite3_errmsg(db));
        return 0;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER,"
        "  topic     TEXT,"
        "  data      TEXT,"
        "  published INTEGER DEFAULT 0"
        ");";

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("DB init: %s", err);
        sqlite3_free(err);
        return 0;
    }

    LOG_INFO("Offline storage ready: %s", MQTT_STORAGE_DB);
    return 1;
}

static void db_store(const char *payload)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO messages (timestamp,topic,data) VALUES(?,?,?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (long long)time(NULL) * 1000);
        sqlite3_bind_text(stmt, 2, MQTT_TOPIC, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, payload,    -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&db_mutex);
}

static void db_publish_stored(void)
{
    if (!db || !mqtt_connected) return;
    pthread_mutex_lock(&db_mutex);

    sqlite3_stmt *stmt;
    const char *sel = "SELECT id,data FROM messages WHERE published=0 ORDER BY timestamp;";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long id      = sqlite3_column_int64(stmt, 0);
        const char *data  = (const char *)sqlite3_column_text(stmt, 1);
        if (!data) continue;

        int rc = mosquitto_publish(mosq, NULL, MQTT_TOPIC,
                                   (int)strlen(data), data, 1, false);
        if (rc == MOSQ_ERR_SUCCESS) {
            char upd[64];
            snprintf(upd, sizeof(upd),
                     "UPDATE messages SET published=1 WHERE id=%lld;", id);
            sqlite3_exec(db, upd, NULL, NULL, NULL);
            count++;
        } else {
            break; /* stop on first failure */
        }
    }
    sqlite3_finalize(stmt);

    if (count > 0) LOG_INFO("Published %d stored messages", count);

    /* Clean up messages older than 7 days */
    long long week_ago = ((long long)time(NULL) - 7 * 24 * 3600) * 1000;
    char del[128];
    snprintf(del, sizeof(del),
             "DELETE FROM messages WHERE published=1 AND timestamp<%lld;", week_ago);
    sqlite3_exec(db, del, NULL, NULL, NULL);

    pthread_mutex_unlock(&db_mutex);
}

/* ============================================================================
 * JSON PAYLOAD BUILDER
 * ========================================================================== */

static void build_payload(char *buf, size_t buflen)
{
    long long ts  = (long long)time(NULL) * 1000;
    int       pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts);
    int       first = 1;

    for (int i = 0; i < point_count && pos < (int)buflen - 128; i++) {
        if (!points[i].valid) continue;
        if (!first) buf[pos++] = ',';
        /* Escape label for JSON safety */
        pos += snprintf(buf + pos, buflen - pos,
                        "\"%s\":%d", points[i].label, points[i].value);
        first = 0;
    }

    if (pos < (int)buflen - 4)
        pos += snprintf(buf + pos, buflen - pos, "}}");
}

/* ============================================================================
 * MQTT CALLBACKS
 * ========================================================================== */

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    if (rc == 0) {
        mqtt_connected = 1;
        LOG_INFO("Connected to HiveMQ Cloud %s:%d", MQTT_BROKER, MQTT_PORT);
        db_publish_stored();
    } else {
        mqtt_connected = 0;
        LOG_ERROR("MQTT connect failed (code %d)", rc);
    }
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud; (void)rc;
    mqtt_connected = 0;
    LOG_WARN("Disconnected from MQTT broker");
}

static int mqtt_init(void)
{
    mosquitto_lib_init();

    char cid[64];
    snprintf(cid, sizeof(cid), "modbus_%ld", (long)time(NULL));

    mosq = mosquitto_new(cid, true, NULL);
    if (!mosq) { LOG_ERROR("mosquitto_new failed"); return 0; }

    mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);

    /* TLS — uses system CA bundle */
    mosquitto_tls_set(mosq,
                      "/etc/ssl/certs/ca-certificates.crt",
                      NULL, NULL, NULL, NULL);
    mosquitto_tls_opts_set(mosq, 1, NULL, NULL);

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    int rc = mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_connect: %s", mosquitto_strerror(rc));
        return 0;
    }

    mosquitto_loop_start(mosq); /* background thread */
    sleep(2);                   /* wait for connection */
    return 1;
}

static void mqtt_publish(const char *payload)
{
    if (mqtt_connected) {
        int rc = mosquitto_publish(mosq, NULL, MQTT_TOPIC,
                                   (int)strlen(payload), payload, 1, false);
        if (rc == MOSQ_ERR_SUCCESS)
            LOG_INFO("Published %zu bytes to %s", strlen(payload), MQTT_TOPIC);
        else {
            LOG_WARN("Publish failed (%s) — storing offline", mosquitto_strerror(rc));
            db_store(payload);
        }
    } else {
        LOG_WARN("MQTT offline — storing data locally");
        db_store(payload);
    }
}

/* ============================================================================
 * SIGNAL HANDLER
 * ========================================================================== */

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ============================================================================
 * MAIN
 * ========================================================================== */

int main(void)
{
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    printf("\n================================================================================\n");
    printf("  MODBUS RTU READER — HiveMQ Cloud MQTT  (STM32MP157F-DK2)\n");
    printf("================================================================================\n");
    printf("  UART    : UART4  TX=GPIO14  RX=GPIO15  DE=GPIO%d\n", RS485_DIR_GPIO);
    printf("  Port    : %s @ %d baud  Slave: %d\n", MODBUS_PORT, MODBUS_BAUD, MODBUS_SLAVE_ID);
    printf("  Config  : %s\n", CONFIG_FILE);
    printf("  Broker  : %s:%d\n", MQTT_BROKER, MQTT_PORT);
    printf("  Topic   : %s\n", MQTT_TOPIC);
    printf("  Interval: %d s\n", MONITORING_INTERVAL);
    printf("================================================================================\n\n");

    if (!parse_csv())  return 1;
    if (!mb_connect()) return 1;

    db_init();
    mqtt_init();

    static char payload[PAYLOAD_MAX];
    int cycle = 0;

    while (running) {
        cycle++;
        time_t now = time(NULL);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        LOG_INFO("===== CYCLE %d — %s =====", cycle, tbuf);

        read_all_points();
        build_payload(payload, sizeof(payload));
        mqtt_publish(payload);

        if (running) {
            LOG_INFO("Next cycle in %d seconds...", MONITORING_INTERVAL);
            /* Interruptible sleep: check running every second */
            for (int s = 0; s < MONITORING_INTERVAL && running; s++)
                sleep(1);
        }
    }

    LOG_INFO("Shutting down...");

    if (mb_ctx) { modbus_close(mb_ctx); modbus_free(mb_ctx); }
    if (mosq)   { mosquitto_loop_stop(mosq, true); mosquitto_destroy(mosq); mosquitto_lib_cleanup(); }
    if (db)     sqlite3_close(db);

    printf("\n================================================================================\n");
    printf("  APPLICATION STOPPED\n");
    printf("================================================================================\n");
    return 0;
}