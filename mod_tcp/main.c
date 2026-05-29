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
#define MODBUS_START_ADDR       0       /* address 1 = index 0 (0-based) */
#define MODBUS_NUM_REGS         100     /* read registers 1..100          */
#define MODBUS_RESPONSE_TIMEOUT 2       /* seconds                        */
#define POLL_INTERVAL_SEC       50       /* polling interval               */

/* SQLite DB path on the target filesystem */
#define DB_PATH "/tmp/modbus_data.db"

/* MQTT broker (loopback if no broker is available) */
#define MQTT_BROKER_HOST "localhost"
#define MQTT_BROKER_PORT 1883
#define MQTT_TOPIC_BASE  "modbus/holding"
#define MQTT_CLIENT_ID   "stm32mp157_modbus_master"

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
        printf("[MQTT] Connected to broker %s:%d\n",
               MQTT_BROKER_HOST, MQTT_BROKER_PORT);
        ctx->mqtt_connected = 1;
    } else {
        fprintf(stderr, "[MQTT] Connection failed (rc=%d)\n", rc);
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

static int mqtt_init(master_ctx_t *ctx)
{
    mosquitto_lib_init();

    ctx->mqtt = mosquitto_new(MQTT_CLIENT_ID, true, ctx);
    if (!ctx->mqtt) {
        fprintf(stderr, "[MQTT] Failed to create client instance\n");
        return -1;
    }

    mosquitto_connect_callback_set(ctx->mqtt, mqtt_on_connect);
    mosquitto_disconnect_callback_set(ctx->mqtt, mqtt_on_disconnect);

    int rc = mosquitto_connect(ctx->mqtt,
                               MQTT_BROKER_HOST, MQTT_BROKER_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Connect error: %s (will continue without MQTT)\n",
                mosquitto_strerror(rc));
        /* Non-fatal – keep running without MQTT */
        return 0;
    }

    /* Start network loop in background thread */
    mosquitto_loop_start(ctx->mqtt);
    return 0;
}

static void mqtt_publish_register(master_ctx_t *ctx, int address,
                                   uint16_t value)
{
    if (!ctx->mqtt_connected)
        return;

    char topic[128], payload[32];
    snprintf(topic,   sizeof(topic),   "%s/%d", MQTT_TOPIC_BASE, address);
    snprintf(payload, sizeof(payload), "%u",    (unsigned int)value);

    int rc = mosquitto_publish(ctx->mqtt, NULL, topic,
                               (int)strlen(payload), payload, 1, false);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[MQTT] Publish failed (addr=%d): %s\n",
                address, mosquitto_strerror(rc));
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

    /* Set response timeout */
    modbus_set_response_timeout(ctx, MODBUS_RESPONSE_TIMEOUT, 0);

    /* Optional: enable debug output by uncommenting below */
    /* modbus_set_debug(ctx, TRUE); */

    if (modbus_set_slave(ctx, slave_id) == -1) {
        fprintf(stderr, "[MB  ] Invalid slave id %d: %s\n",
                slave_id, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }

    /* modbus_connect() here is the libmodbus API call — not recursive */
    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "[MB  ] Connection to %s:%d failed: %s\n",
                ip, port, modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }

    printf("[MB  ] Connected to slave %s:%d  (id=%d)\n", ip, port, slave_id);
    return ctx;
}

/* Read MODBUS_NUM_REGS holding registers starting at MODBUS_START_ADDR */
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
        int addr = MODBUS_START_ADDR + i + 1;   /* display as 1-based */
        printf("  %-6d  %-8u  0x%04X\n",
               addr, (unsigned int)regs[i], (unsigned int)regs[i]);

        db_insert_register(ctx->db, addr, regs[i]);
        mqtt_publish_register(ctx, addr, regs[i]);
    }

    sqlite3_exec(ctx->db, "COMMIT;", NULL, NULL, NULL);

    printf("  ──────────────────────────────────────\n");
    printf("[MB  ] %d registers read and stored.\n", rc);
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
    printf("[CFG ] Poll every : %d seconds\n\n", POLL_INTERVAL_SEC);

    /* Initialise subsystems */
    if (db_init(&ctx.db) != 0)
        return EXIT_FAILURE;

    mqtt_init(&ctx);   /* Non-fatal if broker not reachable */

    ctx.ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
    if (!ctx.ctx) {
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    /* ── Polling loop ── */
    int poll_count = 0;
    while (1) {
        printf("\n[POLL] Cycle #%d\n", ++poll_count);

        int rc = read_holding_registers(&ctx);

        if (rc == -1) {
            /* Attempt reconnect once on failure */
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
