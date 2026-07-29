#include "mb_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <modbus/modbus.h>
#include <sqlite3.h>

/**
 * @brief Modbus TCP master context.
 *
 * Stores the Modbus connection information and SQLite database handle
 * used by the Modbus TCP polling thread.
 */
typedef struct {
    char              slave_ip[64];   /**< Slave IP address. */
    int               slave_port;     /**< Modbus TCP port number. */
    int               slave_id;       /**< Modbus slave ID (Unit ID). */
    modbus_t         *mb_ctx;         /**< Modbus connection context. */
    sqlite3          *db;             /**< SQLite database handle. */
} master_ctx_t;

/**
 * @brief Initializes the SQLite database.
 *
 * Opens the database and creates the holding_registers table if it
 * does not already exist.
 *
 * @param db Pointer to the SQLite database handle.
 *
 * @return 0 on success, -1 on failure.
 */
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

    printf("[ MODBUS_TCP ] Initialised → %s\n", DB_PATH);
    return 0;
}

/**
 * @brief Inserts a holding register value into the database.
 *
 * Stores the register address, value, and current timestamp in the
 * holding_registers table.
 *
 * @param db SQLite database handle.
 * @param address Holding register address.
 * @param value Register value.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Creates a Modbus TCP connection.
 *
 * Allocates a Modbus TCP context, configures the response timeout and
 * slave ID, and establishes a TCP connection to the slave.
 *
 * @param ip Slave IP address.
 * @param port Modbus TCP port.
 * @param slave_id Modbus slave (Unit) ID.
 *
 * @return Pointer to a valid Modbus context on success, or NULL on failure.
 */
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

/**
 * @brief Reads holding registers from the Modbus slave.
 *
 * Reads a block of holding registers, prints the values to the console,
 * and stores them in the SQLite database.
 *
 * @param ctx Pointer to the master context.
 *
 * @return Number of registers read on success, or -1 on failure.
 */
static int read_holding_registers(master_ctx_t *ctx)
{
    uint16_t regs[MODBUS_NUM_REGS];

    int rc = modbus_read_registers(ctx->mb_ctx,
                                   MODBUS_START_ADDR,
                                   MODBUS_NUM_REGS,
                                   regs);
    if (rc == -1) {
        fprintf(stderr, "[MB  ] Read error: %s\n", modbus_strerror(errno));
        return -1;
    }

    printf("\n[ MODBUS_TCP ] ── Holding Registers (addr 1–%d) ──────────────────\n",
           MODBUS_NUM_REGS);
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
    printf("[ MODBUS_TCP ] %d registers read and stored.\n", rc);

    return rc;
}

/**
 * @brief Releases Modbus and database resources.
 *
 * Closes the Modbus TCP connection and the SQLite database if they
 * have been successfully opened.
 *
 * @param ctx Pointer to the master context.
 */
static void cleanup(master_ctx_t *ctx)
{
    if (ctx->mb_ctx) {
        modbus_close(ctx->mb_ctx);
        modbus_free(ctx->mb_ctx);
        printf("[ MODBUS_TCP ] Disconnected.\n");
    }

    if (ctx->db) {
        sqlite3_close(ctx->db);
        printf("[ MODBUS_TCP ] Closed.\n");
    }
}

/**
 * @brief Modbus TCP polling thread.
 *
 * Initializes the SQLite database, connects to the Modbus TCP slave,
 * periodically reads holding registers, stores the values in the
 * database, and automatically attempts to reconnect if communication
 * is lost.
 *
 * @param arg Pointer to an mb_thread_arg_t structure containing the
 *            Modbus TCP configuration.
 *
 * @return Always returns NULL when the thread exits.
 */
void *mb_thread_func1(void *arg)
{
    mb_thread_arg_t *targ = (mb_thread_arg_t *)arg;

    /* ── Validate argument ── */
    if (!targ || targ->slave_ip[0] == '\0') {
        fprintf(stderr, "[ MODBUS_TCP ] mb_thread_func: slave_ip not set in mb_thread_arg_t\n");
        return NULL;
    }

    /* ── Build internal context ── */
    master_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    strncpy(ctx.slave_ip, targ->slave_ip, sizeof(ctx.slave_ip) - 1);
    ctx.slave_port = (targ->slave_port > 0) ? targ->slave_port : MODBUS_DEFAULT_PORT;
    ctx.slave_id   = (targ->slave_id   > 0) ? targ->slave_id   : MODBUS_DEFAULT_SLAVE_ID;
    printf("[ MODBUS_TCP ] modbus tcp start    : %s\n", ctx.slave_ip);
    printf("[ MODBUS_TCP ] Slave IP   : %s\n", ctx.slave_ip);
    printf("[ MODBUS_TCP ] Slave Port : %d\n", ctx.slave_port);
    printf("[ MODBUS_TCP ] Slave ID   : %d\n", ctx.slave_id);
    printf("[ MODBUS_TCP ] Registers  : 1 to %d\n", MODBUS_NUM_REGS);
    printf("[ MODBUS_TCP ] Poll every : %d seconds\n\n", POLL_INTERVAL_SEC);

    /* ── SQLite init ── */
    if (db_init(&ctx.db) != 0) {
        fprintf(stderr, "[ MODBUS_TCP ] Thread exiting: DB init failed.\n");
        return NULL;
    }

    /* ── Modbus connect ── */
    ctx.mb_ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
    if (!ctx.mb_ctx) {
        fprintf(stderr, "[ MODBUS_TCP ] Thread exiting: initial Modbus connect failed.\n");
        //if master cant connect to slave tcp_modbus so thrade end here
        cleanup(&ctx);
        return NULL;
    }

    /* ── Poll loop ── */
    int poll_count = 0;
    while (1) {
        printf("\n[POLL] Cycle #%d\n", ++poll_count);

        int rc = read_holding_registers(&ctx);

        if (rc == -1) {
            fprintf(stderr, "[ MODBUS_TCP ] Attempting reconnect…\n");
            modbus_close(ctx.mb_ctx);
            modbus_free(ctx.mb_ctx);
            ctx.mb_ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
            if (!ctx.mb_ctx) {
                fprintf(stderr, "[ MODBUS_TCP ] Reconnect failed. Thread exiting.\n");
                break;
            }
        }

        sleep(POLL_INTERVAL_SEC);
    }

    cleanup(&ctx);
    return NULL;
}
