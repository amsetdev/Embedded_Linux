/*
 * Modbus TCP/IP Slave - STM32MP157F-DK2
 * Serves Holding Registers address 1 to 100
 *
 * Compile (WSL / arm cross-compiler):
 *   arm-linux-gnueabihf-gcc -O2 -o slave slave.c \
 *       -lmodbus -lmosquitto -lsqlite3 -lpthread
 *
 * Run on target:
 *   ./slave [port] [slave_id]
 *   ./slave              # defaults: port=502, id=1
 *   ./slave 1502 2       # custom port and id
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>

#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* ─── Configuration ──────────────────────────────────────────────── */
#define MODBUS_DEFAULT_PORT      502
#define MODBUS_DEFAULT_SLAVE_ID  1
#define MODBUS_START_ADDR        0
#define MODBUS_NUM_REGS          100
#define MODBUS_MAX_CLIENTS       5       /* max simultaneous TCP connections */

/* SQLite DB path */
#define DB_PATH                  "/tmp/modbus_slave_data.db"

/* ─── MQTT / HiveMQ Cloud credentials ───────────────────────────── */
#define MQTT_BROKER              "03040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT                8883
#define MQTT_TOPIC               "modbus/data"
#define MQTT_TOPIC_CMD           "modbus/cmd"          /* subscribe for writes */
#define MQTT_USERNAME            "prasad"
#define MQTT_PASSWORD            "prasad#12$A"
#define MQTT_CLIENT_ID           "stm32mp157_modbus_slave"
#define MQTT_KEEPALIVE           60

/* Register simulation */
#define SIM_UPDATE_INTERVAL_SEC  5       /* how often simulated values change */

/* ─── Globals ────────────────────────────────────────────────────── */
static volatile int g_running = 1;      /* set to 0 by SIGINT/SIGTERM */

/* ─── Structures ─────────────────────────────────────────────────── */
typedef struct {
    int              slave_port;
    int              slave_id;
    modbus_t        *ctx;               /* server (listening) context   */
    modbus_mapping_t *map;              /* register map                 */
    sqlite3         *db;
    struct mosquitto *mqtt;
    int              mqtt_connected;
    pthread_mutex_t  map_lock;          /* protect map from concurrent access */
} slave_ctx_t;

/* ─── Signal handler ─────────────────────────────────────────────── */
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
        "CREATE TABLE IF NOT EXISTS register_writes ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp TEXT    NOT NULL,"
        "  address   INTEGER NOT NULL,"
        "  value     INTEGER NOT NULL,"
        "  source    TEXT    NOT NULL"   /* 'modbus' or 'mqtt' */
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

static void db_log_write(sqlite3 *db, int address, uint16_t value,
                          const char *source)
{
    char sql[300];
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    snprintf(sql, sizeof(sql),
             "INSERT INTO register_writes (timestamp, address, value, source) "
             "VALUES ('%s', %d, %u, '%s');",
             ts, address, (unsigned int)value, source);

    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB ] Insert failed (addr=%d): %s\n",
                address, err_msg);
        sqlite3_free(err_msg);
    }
}

/* ─── MQTT helpers ───────────────────────────────────────────────── */

/*
 * MQTT command message format (topic: modbus/cmd):
 *   {"addr":<1-based address>,"val":<0-65535>}
 *
 * Example:  {"addr":5,"val":1234}
 *   → writes 1234 into holding register 5
 */
static void mqtt_on_message(struct mosquitto *mosq, void *userdata,
                             const struct mosquitto_message *msg)
{
    slave_ctx_t *ctx = (slave_ctx_t *)userdata;
    (void)mosq;

    if (!msg->payload || msg->payloadlen <= 0)
        return;

    /* Simple JSON parse: look for "addr" and "val" keys */
    char *payload = (char *)msg->payload;
    int   addr = -1;
    unsigned int val = 0;

    char *p;
    if ((p = strstr(payload, "\"addr\"")) != NULL) {
        p += strlen("\"addr\"");
        while (*p == ':' || *p == ' ') p++;
        addr = atoi(p);
    }
    if ((p = strstr(payload, "\"val\"")) != NULL) {
        p += strlen("\"val\"");
        while (*p == ':' || *p == ' ') p++;
        val = (unsigned int)atoi(p);
    }

    if (addr < 1 || addr > MODBUS_NUM_REGS) {
        fprintf(stderr, "[MQTT] CMD ignored – addr %d out of range\n", addr);
        return;
    }

    int idx = addr - 1;   /* convert to 0-based */

    pthread_mutex_lock(&ctx->map_lock);
    ctx->map->tab_registers[idx] = (uint16_t)val;
    pthread_mutex_unlock(&ctx->map_lock);

    printf("[MQTT] CMD → reg[%d] = %u\n", addr, val);
    db_log_write(ctx->db, addr, (uint16_t)val, "mqtt");
}

static void mqtt_on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    slave_ctx_t *ctx = (slave_ctx_t *)userdata;
    if (rc == 0) {
        printf("[MQTT] Connected to HiveMQ Cloud  %s:%d\n",
               MQTT_BROKER, MQTT_PORT);
        ctx->mqtt_connected = 1;

        /* Subscribe to command topic */
        int ret = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_CMD, 1);
        if (ret != MOSQ_ERR_SUCCESS)
            fprintf(stderr, "[MQTT] Subscribe to %s failed: %s\n",
                    MQTT_TOPIC_CMD, mosquitto_strerror(ret));
        else
            printf("[MQTT] Subscribed → %s\n", MQTT_TOPIC_CMD);
    } else {
        fprintf(stderr, "[MQTT] Connection failed (rc=%d): %s\n",
                rc, mosquitto_strerror(rc));
        ctx->mqtt_connected = 0;
    }
}

static void mqtt_on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    slave_ctx_t *ctx = (slave_ctx_t *)userdata;
    (void)mosq;
    ctx->mqtt_connected = 0;
    if (rc != 0)
        fprintf(stderr, "[MQTT] Unexpected disconnect (rc=%d)\n", rc);
}

static int mqtt_init(slave_ctx_t *ctx)
{
    mosquitto_lib_init();

    ctx->mqtt = mosquitto_new(MQTT_CLIENT_ID, true, ctx);
    if (!ctx->mqtt) {
        fprintf(stderr, "[MQTT] Failed to create client instance\n");
        return -1;
    }

    mosquitto_connect_callback_set(ctx->mqtt, mqtt_on_connect);
    mosquitto_disconnect_callback_set(ctx->mqtt, mqtt_on_disconnect);
    mosquitto_message_callback_set(ctx->mqtt, mqtt_on_message);

    /* ── Credentials ── */
    int rc = mosquitto_username_pw_set(ctx->mqtt, MQTT_USERNAME, MQTT_PASSWORD);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Failed to set credentials: %s\n",
                mosquitto_strerror(rc));
        mosquitto_destroy(ctx->mqtt);
        ctx->mqtt = NULL;
        return -1;
    }

    /* ── TLS ── */
    rc = mosquitto_tls_set(ctx->mqtt,
                           "/etc/ssl/certs/ca-certificates.crt",
                           NULL, NULL, NULL, NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] TLS setup failed: %s\n",
                mosquitto_strerror(rc));
        mosquitto_destroy(ctx->mqtt);
        ctx->mqtt = NULL;
        return -1;
    }
    mosquitto_tls_opts_set(ctx->mqtt, 1, "tlsv1.2", NULL);

    /* ── Connect ── */
    rc = mosquitto_connect(ctx->mqtt, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Connect error: %s  (continuing without MQTT)\n",
                mosquitto_strerror(rc));
        return 0;   /* non-fatal */
    }

    mosquitto_loop_start(ctx->mqtt);
    printf("[MQTT] Connecting to HiveMQ Cloud (async)…\n");
    return 0;
}

/*
 * Publish current register snapshot (same JSON format as master)
 * Topic: modbus/data
 */
static void mqtt_publish_snapshot(slave_ctx_t *ctx)
{
    if (!ctx->mqtt_connected)
        return;

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    char payload[2048];
    int  pos = 0;

    pos += snprintf(payload + pos, sizeof(payload) - pos,
                    "{\"ts\":\"%s\",\"slave\":\"self\",\"regs\":[", ts);

    pthread_mutex_lock(&ctx->map_lock);
    for (int i = 0; i < MODBUS_NUM_REGS; i++) {
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "%u%s",
                        (unsigned int)ctx->map->tab_registers[i],
                        (i < MODBUS_NUM_REGS - 1) ? "," : "");
    }
    pthread_mutex_unlock(&ctx->map_lock);

    pos += snprintf(payload + pos, sizeof(payload) - pos, "]}");

    int rc = mosquitto_publish(ctx->mqtt, NULL, MQTT_TOPIC,
                               pos, payload, 1, false);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[MQTT] Publish failed: %s\n",
                mosquitto_strerror(rc));
    else
        printf("[MQTT] Snapshot published → %s\n", MQTT_TOPIC);
}

/* ─── Register simulation thread ─────────────────────────────────── */
/*
 * Periodically updates registers with simulated sensor-like values
 * so the master always has fresh data to read.
 *
 * Register layout (1-based):
 *   1       – uptime seconds (wraps at 65535)
 *   2       – simulated temperature × 10  (e.g. 256 → 25.6 °C)
 *   3       – simulated humidity × 10
 *   4       – simulated voltage  × 100 (e.g. 2450 → 24.50 V)
 *   5-100   – counter / ramp value  (addr × poll_count, masked to 16-bit)
 */
static void *sim_thread(void *arg)
{
    slave_ctx_t *ctx = (slave_ctx_t *)arg;
    uint32_t tick = 0;

    while (g_running) {
        sleep(SIM_UPDATE_INTERVAL_SEC);
        tick++;

        pthread_mutex_lock(&ctx->map_lock);

        /* reg 1 – uptime (seconds, wraps) */
        ctx->map->tab_registers[0] =
            (uint16_t)((tick * SIM_UPDATE_INTERVAL_SEC) & 0xFFFF);

        /* reg 2 – temperature: 20.0–35.0 °C, encoded ×10 */
        ctx->map->tab_registers[1] =
            (uint16_t)(200 + (tick * 7) % 150);

        /* reg 3 – humidity: 40–90 %, encoded ×10 */
        ctx->map->tab_registers[2] =
            (uint16_t)(400 + (tick * 13) % 500);

        /* reg 4 – voltage: 22.00–26.00 V, encoded ×100 */
        ctx->map->tab_registers[3] =
            (uint16_t)(2200 + (tick * 11) % 400);

        /* regs 5-100 – ramp counters */
        for (int i = 4; i < MODBUS_NUM_REGS; i++)
            ctx->map->tab_registers[i] =
                (uint16_t)(((i + 1) * tick) & 0xFFFF);

        pthread_mutex_unlock(&ctx->map_lock);

        printf("[SIM ] Tick #%u – registers updated\n", tick);
    }

    return NULL;
}

/* ─── Modbus server ──────────────────────────────────────────────── */

static int mb_server_init(slave_ctx_t *ctx)
{
    /* Allocate register map:
     *   tab_bits            – coils
     *   tab_input_bits      – discrete inputs
     *   tab_input_registers – input registers
     *   tab_registers       – holding registers  ← we use these
     */
    ctx->map = modbus_mapping_new(0, 0, MODBUS_NUM_REGS, MODBUS_NUM_REGS);
    if (!ctx->map) {
        fprintf(stderr, "[MB  ] Failed to allocate register map: %s\n",
                modbus_strerror(errno));
        return -1;
    }

    /* Pre-fill holding registers with recognisable default values */
    for (int i = 0; i < MODBUS_NUM_REGS; i++)
        ctx->map->tab_registers[i] = (uint16_t)(i + 1);   /* 1,2,…,100 */

    /* Create TCP server context */
    ctx->ctx = modbus_new_tcp(NULL, ctx->slave_port);
    if (!ctx->ctx) {
        fprintf(stderr, "[MB  ] modbus_new_tcp failed: %s\n",
                modbus_strerror(errno));
        return -1;
    }

    if (modbus_set_slave(ctx->ctx, ctx->slave_id) == -1) {
        fprintf(stderr, "[MB  ] Invalid slave id %d\n", ctx->slave_id);
        return -1;
    }

    /* Uncomment to enable verbose libmodbus output */
    /* modbus_set_debug(ctx->ctx, TRUE); */

    printf("[MB  ] Slave listening on port %d  (id=%d)\n",
           ctx->slave_port, ctx->slave_id);
    return 0;
}

/*
 * Log every register written by a Modbus master into SQLite.
 * Called after modbus_reply() returns successfully.
 */
static void log_modbus_write(slave_ctx_t *ctx,
                              const uint8_t *query, int query_len)
{
    if (query_len < 6) return;

    uint8_t fc = query[modbus_get_header_length(ctx->ctx)];

    /* FC 06 – Write Single Register */
    if (fc == MODBUS_FC_WRITE_SINGLE_REGISTER) {
        int addr  = (query[modbus_get_header_length(ctx->ctx) + 1] << 8)
                  |  query[modbus_get_header_length(ctx->ctx) + 2];
        int value = (query[modbus_get_header_length(ctx->ctx) + 3] << 8)
                  |  query[modbus_get_header_length(ctx->ctx) + 4];
        db_log_write(ctx->db, addr + 1, (uint16_t)value, "modbus");
        printf("[MB  ] FC06 write → reg[%d] = %d\n", addr + 1, value);
    }
    /* FC 16 – Write Multiple Registers */
    else if (fc == MODBUS_FC_WRITE_MULTIPLE_REGISTERS) {
        int addr  = (query[modbus_get_header_length(ctx->ctx) + 1] << 8)
                  |  query[modbus_get_header_length(ctx->ctx) + 2];
        int count = (query[modbus_get_header_length(ctx->ctx) + 3] << 8)
                  |  query[modbus_get_header_length(ctx->ctx) + 4];
        printf("[MB  ] FC16 write → reg[%d..%d] (%d regs)\n",
               addr + 1, addr + count, count);
        /* Individual values already applied by libmodbus before reply */
        for (int i = 0; i < count; i++)
            db_log_write(ctx->db, addr + 1 + i,
                         ctx->map->tab_registers[addr + i], "modbus");
    }
}

/* ─── MQTT snapshot publisher thread ─────────────────────────────── */
/*
 * Publishes a register snapshot to HiveMQ Cloud every
 * SIM_UPDATE_INTERVAL_SEC seconds, independent of Modbus activity.
 */
static void *mqtt_pub_thread(void *arg)
{
    slave_ctx_t *ctx = (slave_ctx_t *)arg;

    while (g_running) {
        sleep(SIM_UPDATE_INTERVAL_SEC);
        mqtt_publish_snapshot(ctx);
    }

    return NULL;
}

/* ─── Cleanup ────────────────────────────────────────────────────── */

static void cleanup(slave_ctx_t *ctx)
{
    if (ctx->ctx) {
        modbus_close(ctx->ctx);
        modbus_free(ctx->ctx);
        printf("[MB  ] Server closed.\n");
    }
    if (ctx->map) {
        modbus_mapping_free(ctx->map);
        printf("[MB  ] Register map freed.\n");
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
    pthread_mutex_destroy(&ctx->map_lock);
}

/* ─── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    slave_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.map_lock, NULL);

    ctx.slave_port = (argc >= 2) ? atoi(argv[1]) : MODBUS_DEFAULT_PORT;
    ctx.slave_id   = (argc >= 3) ? atoi(argv[2]) : MODBUS_DEFAULT_SLAVE_ID;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Modbus TCP/IP Slave  – STM32MP157F-DK2      ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf("[CFG ] Port       : %d\n", ctx.slave_port);
    printf("[CFG ] Slave ID   : %d\n", ctx.slave_id);
    printf("[CFG ] Registers  : 1 to %d\n", MODBUS_NUM_REGS);
    printf("[CFG ] MQTT Broker: %s:%d\n\n", MQTT_BROKER, MQTT_PORT);

    /* ── Init subsystems ── */
    if (db_init(&ctx.db) != 0)
        return EXIT_FAILURE;

    mqtt_init(&ctx);   /* non-fatal if broker unreachable */

    if (mb_server_init(&ctx) != 0) {
        cleanup(&ctx);
        return EXIT_FAILURE;
    }

    /* ── Start background threads ── */
    pthread_t sim_tid, pub_tid;
    pthread_create(&sim_tid, NULL, sim_thread,     &ctx);
    pthread_create(&pub_tid, NULL, mqtt_pub_thread, &ctx);

    /* ── Get the listening socket from libmodbus ── */
    int server_socket = modbus_tcp_listen(ctx.ctx, MODBUS_MAX_CLIENTS);
    if (server_socket == -1) {
        fprintf(stderr, "[MB  ] modbus_tcp_listen failed: %s\n",
                modbus_strerror(errno));
        g_running = 0;
        pthread_join(sim_tid, NULL);
        pthread_join(pub_tid, NULL);
        cleanup(&ctx);
        return EXIT_FAILURE;
    }
    printf("[MB  ] Waiting for master connections…\n\n");

    /* ── Accept / serve loop ── */
    while (g_running) {
        fd_set ref_set, rd_set;
        FD_ZERO(&ref_set);
        FD_SET(server_socket, &ref_set);
        int fd_max = server_socket;

        /* We manage a simple single-client-at-a-time fd set here.
         * For multi-client, extend with an fd array and loop. */
        rd_set = ref_set;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(fd_max + 1, &rd_set, NULL, NULL, &tv);

        if (ready == -1) {
            if (errno == EINTR) continue;   /* interrupted by signal */
            perror("[MB  ] select");
            break;
        }

        if (ready == 0) continue;   /* timeout – check g_running */

        if (FD_ISSET(server_socket, &rd_set)) {
            /* Accept new connection */
            int client_socket = modbus_tcp_accept(ctx.ctx, &server_socket);
            if (client_socket == -1) {
                fprintf(stderr, "[MB  ] Accept failed: %s\n",
                        modbus_strerror(errno));
                continue;
            }
            printf("[MB  ] Master connected (fd=%d)\n", client_socket);

            /* ── Serve this client until it disconnects ── */
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

            while (g_running) {
                int rc = modbus_receive(ctx.ctx, query);

                if (rc > 0) {
                    /* Log any write before replying */
                    log_modbus_write(&ctx, query, rc);

                    pthread_mutex_lock(&ctx.map_lock);
                    modbus_reply(ctx.ctx, query, rc, ctx.map);
                    pthread_mutex_unlock(&ctx.map_lock);

                } else if (rc == -1) {
                    /* Connection closed or error */
                    printf("[MB  ] Master disconnected (fd=%d): %s\n",
                           client_socket, modbus_strerror(errno));
                    break;
                }
            }

            close(client_socket);
        }
    }

    printf("\n[MAIN] Shutting down…\n");
    g_running = 0;

    pthread_join(sim_tid, NULL);
    pthread_join(pub_tid, NULL);

    close(server_socket);
    cleanup(&ctx);
    return EXIT_SUCCESS;
}