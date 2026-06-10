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


typedef struct {
    char              slave_ip[64];
    int               slave_port;
    int               slave_id;
    modbus_t         *mb_ctx;
    sqlite3          *db;
} master_ctx_t;


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

    int rc = modbus_read_registers(ctx->mb_ctx,
                                   MODBUS_START_ADDR,
                                   MODBUS_NUM_REGS,
                                   regs);
    if (rc == -1) {
        fprintf(stderr, "[MB  ] Read error: %s\n", modbus_strerror(errno));
        return -1;
    }

    printf("\n[MB  ] ── Holding Registers (addr 1–%d) ──────────────────\n",
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
    printf("[MB  ] %d registers read and stored.\n", rc);

    return rc;
}

static void cleanup(master_ctx_t *ctx)
{
    if (ctx->mb_ctx) {
        modbus_close(ctx->mb_ctx);
        modbus_free(ctx->mb_ctx);
        printf("[MB  ] Disconnected.\n");
    }
    if (ctx->db) {
        sqlite3_close(ctx->db);
        printf("[DB ] Closed.\n");
    }
}

void *mb_thread_func1(void *arg)
{
    mb_thread_arg_t *targ = (mb_thread_arg_t *)arg;

    /* ── Validate argument ── */
    if (!targ || targ->slave_ip[0] == '\0') {
        fprintf(stderr, "[MB  ] mb_thread_func: slave_ip not set in mb_thread_arg_t\n");
        return NULL;
    }

    /* ── Build internal context ── */
    master_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    strncpy(ctx.slave_ip, targ->slave_ip, sizeof(ctx.slave_ip) - 1);
    ctx.slave_port = (targ->slave_port > 0) ? targ->slave_port : MODBUS_DEFAULT_PORT;
    ctx.slave_id   = (targ->slave_id   > 0) ? targ->slave_id   : MODBUS_DEFAULT_SLAVE_ID;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Modbus TCP/IP Master – STM32MP157F-DK2      ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf("[CFG ] Slave IP   : %s\n", ctx.slave_ip);
    printf("[CFG ] Slave Port : %d\n", ctx.slave_port);
    printf("[CFG ] Slave ID   : %d\n", ctx.slave_id);
    printf("[CFG ] Registers  : 1 to %d\n", MODBUS_NUM_REGS);
    printf("[CFG ] Poll every : %d seconds\n\n", POLL_INTERVAL_SEC);

    /* ── SQLite init ── */
    if (db_init(&ctx.db) != 0) {
        fprintf(stderr, "[MB  ] Thread exiting: DB init failed.\n");
        return NULL;
    }

    /* ── Modbus connect ── */
    ctx.mb_ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
    if (!ctx.mb_ctx) {
        fprintf(stderr, "[MB  ] Thread exiting: initial Modbus connect failed.\n");
        cleanup(&ctx);
        return NULL;
    }

    /* ── Poll loop ── */
    int poll_count = 0;
    while (1) {
        printf("\n[POLL] Cycle #%d\n", ++poll_count);

        int rc = read_holding_registers(&ctx);

        if (rc == -1) {
            fprintf(stderr, "[MB  ] Attempting reconnect…\n");
            modbus_close(ctx.mb_ctx);
            modbus_free(ctx.mb_ctx);
            ctx.mb_ctx = mb_connect(ctx.slave_ip, ctx.slave_port, ctx.slave_id);
            if (!ctx.mb_ctx) {
                fprintf(stderr, "[MB  ] Reconnect failed. Thread exiting.\n");
                break;
            }
        }

        sleep(POLL_INTERVAL_SEC);
    }

    cleanup(&ctx);
    return NULL;
}