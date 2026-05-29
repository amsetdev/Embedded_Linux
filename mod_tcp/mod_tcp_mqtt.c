/*
 * Modbus TCP/IP Master - STM32MP157F-DK2
 * Reads Holding Registers address 1 to 100
 *
 * Compile (WSL / arm cross-compiler):
 *   arm-linux-gnueabihf-gcc -O2 -o main main.c \
 *       -lmodbus -lmosquitto -lsqlite3 -lpthread
 *
 * Run on target:
 *   ./main <slave_ip> [slave_id] [port]
 *   ./main 192.168.1.10          # defaults: id=1, port=502
 *   ./main 192.168.1.10 2 1502   # custom id and port
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* ─── Configuration ──────────────────────────────────────────────── */
#define MODBUS_DEFAULT_PORT     502
#define MODBUS_DEFAULT_SLAVE_ID 1
#define MODBUS_START_ADDR       0
#define MODBUS_NUM_REGS         100
#define MODBUS_RESPONSE_TIMEOUT 2
#define POLL_INTERVAL_SEC       120

/* SQLite DB path on the target filesystem */
#define DB_PATH                "/tmp/modbus_data.db"

/* ─── MQTT / HiveMQ Cloud credentials ───────────────────────────── */
#define MQTT_BROKER            "3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT              8883
#define MQTT_TOPIC             "modbus/data"
#define MQTT_USERNAME          "prasad"
#define MQTT_PASSWORD          "prasad#12$A"
#define MQTT_CLIENT_ID         "stm32mp157_modbus_master"
#define MQTT_KEEPALIVE         60

/* ─── Structures ─────────────────────────────────────────────────── */
typedef struct {
    char        slave_ip[64];
    int         slave_port;
    int         slave_id;
    modbus_t   *ctx;
    sqlite3    *db;
    struct mosquitto *mqtt;
    int         mqtt_connected;
} master_ctx_t;

/* ─── SQLite helpers ─────────────────────────────────────────────── */

static int db_init(sqlite3 **db)
{
    int rc = sqlite3_open(DB_PATH, db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB ] Cannot open database: %s\n",
                sqlite3_errmsg(*db));
        return -1;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS holding_registers ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp TEXT    NOT NULL,"
        "  address   INTEGER NOT NULL,"
        "  value     INTEGER NOT NULL"
        ");";

    char *err_msg = NULL;
    rc = sqlite3_exec(*db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB ] Table creation failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    printf("[DB ] Initialised → %s\n", DB_PATH);
    return 0;
}

static int db_insert_register(sqlite3 *db, int address, uint16_t value)
{
    char sql[256];
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    snprintf(sql, sizeof(sql),
             "INSERT INTO holding_registers (timestamp, address, value) "
             "VALUES ('%s', %d, %u);",
             ts, address, (unsigned int)value);

    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB ] Insert failed (addr=%d): %s\n",
                address, err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}

/* ─── MQTT helpers ───────────────────────────────────────────────── */

static void mqtt_on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    master_ctx_t *ctx = (master_ctx_t *)userdata;
    if (rc == 0) {
        printf("[MQTT] Connected to HiveMQ Cloud  %s:%d\n",
               MQTT_BROKER, MQTT_PORT);
        ctx->mqtt_connected = 1;
    } else {
        fprintf(stderr, "[MQTT] Connection failed (rc=%d): %s\n",
                rc, mosquitto_strerror(rc));
        ctx->mqtt_connected = 0;
    }
}

static void mqtt_on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    master_ctx_t *ctx = (master_ctx_t *)userdata;
    ctx->mqtt_connected = 0;
    if (rc != 0)
        fprintf(stderr, "[MQTT] Unexpected disconnect (rc=%d)\n", rc);
}

static void mqtt_on_log(struct mosquitto *mosq, void *userdata,
                        int level, const char *str)
{
    /* Uncomment to see verbose mosquitto debug output */
    /* printf("[MQTT][log] %s\n", str); */
    (void)mosq; (void)userdata; (void)level; (void)str;
}

static int mqtt_init(master_ctx_t *ctx)
{
    mosquitto_lib_init();

    ctx->mqtt = mosquitto_new(MQTT_CLIENT_ID, true, ctx);
    if (!ctx->mqtt) {
        fprintf(stderr, "[MQTT] Failed to create client instance\n");
        return -1;
    }

    /* ── Callbacks ── */
    mosquitto_connect_callback_set(ctx->mqtt, mqtt_on_connect);
    mosquitto_disconnect_callback_set(ctx->mqtt, mqtt_on_disconnect);
    mosquitto_log_callback_set(ctx->mqtt, mqtt_on_log);

    /* ── Credentials ── */
    int rc = mosquitto_username_pw_set(ctx->mqtt, MQTT_USERNAME, MQTT_PASSWORD);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Failed to set credentials: %s\n",
                mosquitto_strerror(rc));
        mosquitto_destroy(ctx->mqtt);
        ctx->mqtt = NULL;
        return -1;
    }

    /* ── TLS – HiveMQ Cloud requires TLS on port 8883 ── */
    /*
     * mosquitto_tls_set(ctx->mqtt,
     *   cafile,    // path to CA bundle, e.g. "/etc/ssl/certs/ca-certificates.crt"
     *   capath,    // OR directory of CA certs  (pass NULL if cafile used)
     *   certfile,  // client cert  (NULL for server-auth only)
     *   keyfile,   // client key   (NULL for server-auth only)
     *   NULL);     // password callback
     *
     * On Debian/Ubuntu the system CA bundle is usually at:
     *   /etc/ssl/certs/ca-certificates.crt
     * On OpenWrt / Yocto it may be at:
     *   /etc/ssl/certs/ca-bundle.crt   or   /etc/ssl/cert.pem
     */
    rc = mosquitto_tls_set(ctx->mqtt,
                           "/etc/ssl/certs/ca-certificates.crt", /* CA file  */
                           NULL,   /* capath  */
                           NULL,   /* certfile (not needed for HiveMQ Cloud) */
                           NULL,   /* keyfile  */
                           NULL);  /* pw callback */
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] TLS setup failed: %s\n",
                mosquitto_strerror(rc));
        fprintf(stderr, "[MQTT] Check that the CA bundle path is correct "
                        "for your target OS.\n");
        mosquitto_destroy(ctx->mqtt);
        ctx->mqtt = NULL;
        return -1;               /* TLS is required for HiveMQ Cloud */
    }

    /* Optional: enforce TLS 1.2 minimum */
    mosquitto_tls_opts_set(ctx->mqtt, 1 /*verify peer*/, "tlsv1.2", NULL);

    /* ── Connect ── */
    rc = mosquitto_connect(ctx->mqtt, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Connect error: %s  (will continue without MQTT)\n",
                mosquitto_strerror(rc));
        /* Non-fatal – Modbus polling keeps running */
        return 0;
    }

    /* Start network loop in a background thread */
    mosquitto_loop_start(ctx->mqtt);
    printf("[MQTT] Connecting to HiveMQ Cloud (async)…\n");
    return 0;
}

/* ─── Publish all 100 registers as a single JSON payload ─────────── */
/*
 * Published topic : modbus/data
 * Payload format  :
 *   {"ts":"2025-05-26T10:00:00","slave":"192.168.1.10","regs":[v1,v2,...,v100]}
 */
static void mqtt_publish_all(master_ctx_t *ctx,
                             const uint16_t *regs, int count)
{
    if (!ctx->mqtt_connected)
        return;

    /* Build JSON payload */
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    /* worst-case size: header + 100 × "65535," + footer */
    char payload[2048];
    int  pos = 0;

    pos += snprintf(payload + pos, sizeof(payload) - pos,
                    "{\"ts\":\"%s\",\"slave\":\"%s\",\"regs\":[",
                    ts, ctx->slave_ip);

    for (int i = 0; i < count && pos < (int)sizeof(payload) - 16; i++) {
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "%u%s",
                        (unsigned int)regs[i],
                        (i < count - 1) ? "," : "");
    }

    pos += snprintf(payload + pos, sizeof(payload) - pos, "]}");

    int rc = mosquitto_publish(ctx->mqtt,
                               NULL,               /* message id (out) */
                               MQTT_TOPIC,
                               pos,                /* payload length   */
                               payload,
                               1,                  /* QoS 1            */
                               false);             /* retain           */
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[MQTT] Publish failed: %s\n",
                mosquitto_strerror(rc));
    else
        printf("[MQTT] Published %d registers → %s\n", count, MQTT_TOPIC);
}

/* ─── Modbus helpers ─────────────────────────────────────────────── */

static modbus_t *mb_connect(const char *ip, int port, int slave_id)
{
    modbus_t *ctx = modbus_new_tcp(ip, port);
    if (!ctx) {
        fprintf(stderr, "[MB  ] Unable to allocate Modbus context: %s\n",
                modbus_strerror(errno));
        return NULL;
    }

    modbus_set_response_timeout(ctx, MODBUS_RESPONSE_TIMEOUT, 0);

    if (modbus_set_slave(ctx, slave_id) == -1) {
        fprintf(stderr, "[MB  ] Invalid slave id %d: %s\n",
                slave_id, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }

    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "[MB  ] Connection to %s:%d failed: %s\n",
                ip, port, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }

    printf("[MB  ] Connected to slave %s:%d  (id=%d)\n", ip, port, slave_id);
    return ctx;
}

static int read_holding_registers(master_ctx_t *ctx)
{
    uint16_t regs[MODBUS_NUM_REGS];

    int rc = modbus_read_registers(ctx->ctx,
                                   MODBUS_START_ADDR,
                                   MODBUS_NUM_REGS,
                                   regs);
    if (rc == -1) {
        fprintf(stderr, "[MB  ] Read error: %s\n", modbus_strerror(errno));
        return -1;
    }

    printf("\n[MB  ] ── Holding Registers (addr 1–100) ──────────────────\n");
    printf("  %-6s  %-8s  %-6s\n", "Addr", "Dec", "Hex");
    printf("  ──────────────────────────────────────\n");

    /* Begin SQLite transaction for bulk insert */
    sqlite3_exec(ctx->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    for (int i = 0; i < rc; i++) {
        int addr = MODBUS_START_ADDR + i + 1;
        printf("  %-6d  %-8u  0x%04X\n",
               addr, (unsigned int)regs[i], (unsigned int)regs[i]);
        db_insert_register(ctx->db, addr, regs[i]);
    }

    sqlite3_exec(ctx->db, "COMMIT;", NULL, NULL, NULL);

    printf("  ──────────────────────────────────────\n");
    printf("[MB  ] %d registers read and stored.\n", rc);

    /* Publish all registers as one JSON message on modbus/data */
    mqtt_publish_all(ctx, regs, rc);

    return rc;
}

/* ─── Cleanup ────────────────────────────────────────────────────── */

static void cleanup(master_ctx_t *ctx)
{
    if (ctx->ctx) {
        modbus_close(ctx->ctx);
        modbus_free(ctx->ctx);
        printf("[MB  ] Disconnected.\n");
    }
    if (ctx->db) {
        sqlite3_close(ctx->db);
        printf("[DB ] Closed.\n");
    }
    if (ctx->mqtt) {
        mosquitto_loop_stop(ctx->mqtt, true);
        mosquitto_destroy(ctx->mqtt);
        mosquitto_lib_cleanup();
        printf("[MQTT] Cleaned up.\n");
    }
}

/* ─── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <slave_ip> [slave_id] [port]\n"
                "  slave_ip   : IP address of the Modbus TCP slave\n"
                "  slave_id   : Modbus unit id  (default: %d)\n"
                "  port       : TCP port        (default: %d)\n\n"
                "Example:\n"
                "  %s 192.168.1.10\n"
                "  %s 192.168.1.10 2 1502\n",
                argv[0],
                MODBUS_DEFAULT_SLAVE_ID,
                MODBUS_DEFAULT_PORT,
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    master_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    strncpy(ctx.slave_ip, argv[1], sizeof(ctx.slave_ip) - 1);
    ctx.slave_id   = (argc >= 3) ? atoi(argv[2]) : MODBUS_DEFAULT_SLAVE_ID;
    ctx.slave_port = (argc >= 4) ? atoi(argv[3]) : MODBUS_DEFAULT_PORT;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Modbus TCP/IP Master – STM32MP157F-DK2      ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf("[CFG ] Slave IP   : %s\n", ctx.slave_ip);
    printf("[CFG ] Slave Port : %d\n", ctx.slave_port);
    printf("[CFG ] Slave ID   : %d\n", ctx.slave_id);
    printf("[CFG ] Registers  : 1 to %d\n", MODBUS_NUM_REGS);
    printf("[CFG ] Poll every : %d seconds\n", POLL_INTERVAL_SEC);
    printf("[CFG ] MQTT Broker: %s:%d\n\n", MQTT_BROKER, MQTT_PORT);

    if (db_init(&ctx.db) != 0)
        return EXIT_FAILURE;

    mqtt_init(&ctx);   /* Non-fatal if broker not reachable */

    ctx.ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
    if (!ctx.ctx) {
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    int poll_count = 0;
    while (1) {
        printf("\n[POLL] Cycle #%d\n", ++poll_count);

        int rc = read_holding_registers(&ctx);

        if (rc == -1) {
            fprintf(stderr, "[MB  ] Attempting reconnect…\n");
            modbus_close(ctx.ctx);
            modbus_free(ctx.ctx);
            ctx.ctx = mb_connect(ctx.slave_ip,
                                  ctx.slave_port,
                                  ctx.slave_id);
            if (!ctx.ctx) {
                fprintf(stderr, "[MB  ] Reconnect failed. Exiting.\n");
                break;
            }
        }

        sleep(POLL_INTERVAL_SEC);
    }

    cleanup(&ctx);
    return EXIT_SUCCESS;
}