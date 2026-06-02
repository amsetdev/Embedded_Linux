/*
 * Modbus TCP/IP Master - STM32MP157F-DK2
 * Reads Holding Registers address 1 to 100
 * MQTT → AWS IoT Core (certificate-based mTLS)
 *
 * Compile (WSL / arm cross-compiler):
 *   arm-linux-gnueabihf-gcc -O2 -o master master.c \
 *       -lmodbus -lmosquitto -lsqlite3 -lpthread
 *
 * Run on target:
 *   ./master <slave_ip> [slave_id] [port]
 *   ./master 192.168.1.10
 *   ./master 192.168.1.10 2 1502
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* ─── Modbus Configuration ───────────────────────────────────────── */
#define MODBUS_DEFAULT_PORT      502
#define MODBUS_DEFAULT_SLAVE_ID  1
#define MODBUS_START_ADDR        0
#define MODBUS_NUM_REGS          100
#define MODBUS_RESPONSE_TIMEOUT  2
#define POLL_INTERVAL_SEC        50

/* ─── SQLite ─────────────────────────────────────────────────────── */
#define DB_PATH                  "/tmp/modbus_master_data.db"

/* ─── MQTT / AWS IoT Core ────────────────────────────────────────── */
#define MQTT_BROKER              "mqtts://a38rhx6jdboq9n-ats.iot.ap-south-1.amazonaws.com"
#define MQTT_PORT                8883
#define MQTT_TOPIC               "modbus/data"
#define MQTT_CLIENT_ID           "stm32mp157_modbus_master"
#define MQTT_KEEPALIVE           60

/* Certificate paths on target filesystem */
#define MQTT_CA_CERT             "/opt/certs/AmazonRootCA1.pem"
#define MQTT_CLIENT_CERT         "/opt/certs/certificate.pem.crt"
#define MQTT_CLIENT_KEY          "/opt/certs/private.pem.key"

/* ─── Structures ─────────────────────────────────────────────────── */
typedef struct {
    char              slave_ip[64];
    int               slave_port;
    int               slave_id;
    modbus_t         *ctx;
    sqlite3          *db;
    struct mosquitto *mqtt;
    int               mqtt_connected;
} master_ctx_t;

/* ─── Signal handler ─────────────────────────────────────────────── */
static volatile int g_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

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
    (void)mosq;
    master_ctx_t *ctx = (master_ctx_t *)userdata;
    if (rc == 0) {
        printf("[MQTT] Connected to AWS IoT Core  %s:%d\n",
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
    (void)mosq;
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

    /* ── Callbacks ── */
    mosquitto_connect_callback_set(ctx->mqtt, mqtt_on_connect);
    mosquitto_disconnect_callback_set(ctx->mqtt, mqtt_on_disconnect);

    /* ── AWS IoT Core: certificate-based mTLS (no username/password) ── */
    int rc = mosquitto_tls_set(ctx->mqtt,
                               MQTT_CA_CERT,      /* AmazonRootCA1.pem     */
                               NULL,              /* capath                */
                               MQTT_CLIENT_CERT,  /* certificate.pem.crt  */
                               MQTT_CLIENT_KEY,   /* private.pem.key       */
                               NULL);             /* key passphrase cb     */
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] TLS setup failed: %s\n",
                mosquitto_strerror(rc));
        fprintf(stderr, "[MQTT] Certificate paths:\n"
                        "         CA  : %s\n"
                        "         CERT: %s\n"
                        "         KEY : %s\n",
                MQTT_CA_CERT, MQTT_CLIENT_CERT, MQTT_CLIENT_KEY);
        mosquitto_destroy(ctx->mqtt);
        ctx->mqtt = NULL;
        return -1;
    }

    /* AWS IoT Core requires TLS 1.2 minimum */
    mosquitto_tls_opts_set(ctx->mqtt, 1, "tlsv1.2", NULL);

    /* ── Connect ── */
    rc = mosquitto_connect(ctx->mqtt, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Connect error: %s  (continuing without MQTT)\n",
                mosquitto_strerror(rc));
        return 0;   /* non-fatal */
    }

    mosquitto_loop_start(ctx->mqtt);
    printf("[MQTT] Connecting to AWS IoT Core (async)…\n");
    return 0;
}

/*
 * Publish all 100 registers as one JSON message
 * Topic  : modbus/data
 * Payload: {"ts":"…","slave":"192.168.1.10","regs":[v1,…,v100]}
 */
static void mqtt_publish_all(master_ctx_t *ctx,
                              const uint16_t *regs, int count)
{
    if (!ctx->mqtt_connected)
        return;

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

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

    int rc = mosquitto_publish(ctx->mqtt, NULL, MQTT_TOPIC,
                               pos, payload, 1, false);
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
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <slave_ip> [slave_id] [port]\n"
                "  slave_ip  : IP address of the Modbus TCP slave\n"
                "  slave_id  : Modbus unit id  (default: %d)\n"
                "  port      : TCP port        (default: %d)\n\n"
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
    printf("[CFG ] Poll every : %d s\n", POLL_INTERVAL_SEC);
    printf("[CFG ] MQTT Broker: %s:%d\n", MQTT_BROKER, MQTT_PORT);
    printf("[CFG ] CA cert    : %s\n", MQTT_CA_CERT);
    printf("[CFG ] Cli cert   : %s\n\n", MQTT_CLIENT_CERT);

    if (db_init(&ctx.db) != 0)
        return EXIT_FAILURE;

    mqtt_init(&ctx);

    ctx.ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
    if (!ctx.ctx) {
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    int poll_count = 0;
    while (g_running) {
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

    printf("\n[MAIN] Shutting down…\n");
    cleanup(&ctx);
    return EXIT_SUCCESS;
}