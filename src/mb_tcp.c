/**
 * @file mb_tcp.c
 * @brief Modbus TCP SLAVE (server) implementation using libmodbus.
 *
 * Listens on a TCP port, accepts one or more master connections
 * (select()-multiplexed, same pattern as libmodbus' own
 * unit-test-server example), and answers FC01/02/03/04/05/06/15/16
 * requests directly from the shared register map (mb_regmap.h) via
 * a modbus_mapping_t that points at the same backing arrays the RTU
 * slave uses. Every accepted write is also logged to SQLite.
 */

#include "mb_tcp.h"
#include "mb_regmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <modbus/modbus.h>
#include <sqlite3.h>

/**
 * @brief Modbus TCP slave context.
 */
typedef struct {
    char     bind_ip[64];
    int      listen_port;
    int      slave_id;
    modbus_t *mb_ctx;
    sqlite3  *db;
} slave_ctx_t;

/**
 * @brief Initializes the SQLite database used to log incoming
 *        write requests from external masters.
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
 * @brief Logs one register write received from a master.
 */
static void db_log_write(sqlite3 *db, int address, uint16_t value)
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
        fprintf(stderr, "[DB ] Insert failed (addr=%d): %s\n", address, err_msg);
        sqlite3_free(err_msg);
    }
}

/**
 * @brief Builds a libmodbus mapping whose backing arrays ARE the
 *        shared register map's arrays, so libmodbus reads/writes
 *        the exact same data the RTU slave serves.
 */
static modbus_mapping_t *build_shared_mapping(void)
{
    modbus_mapping_t *mb_mapping = malloc(sizeof(modbus_mapping_t));
    if (!mb_mapping) return NULL;
    memset(mb_mapping, 0, sizeof(*mb_mapping));

    mb_mapping->nb_bits              = MB_NUM_COILS;
    mb_mapping->nb_input_bits        = MB_NUM_DISCRETE_INPUTS;
    mb_mapping->nb_registers         = MB_NUM_HOLDING_REGS;
    mb_mapping->nb_input_registers   = MB_NUM_INPUT_REGS;

    mb_mapping->tab_bits             = mb_regmap_coils_ptr();
    mb_mapping->tab_input_bits       = mb_regmap_discrete_ptr();
    mb_mapping->tab_registers        = mb_regmap_holding_ptr();
    mb_mapping->tab_input_registers  = mb_regmap_input_ptr();

    return mb_mapping;
}

/**
 * @brief Frees the mapping wrapper WITHOUT freeing the backing
 *        arrays (those belong to mb_regmap.c, not libmodbus).
 */
static void free_shared_mapping(modbus_mapping_t *m)
{
    if (!m) return;
    free(m);
}

/**
 * @brief Logs any holding-register write contained in the just
 *        processed request (FC06/FC16), by diffing wasn't needed —
 *        libmodbus already applied the write to tab_registers before
 *        we get to log it, so we simply log the affected addresses.
 */
static void log_write_if_any(sqlite3 *db, const uint8_t *req, int req_len,
                              modbus_t *ctx)
{
    (void)req_len;
    int hdr = modbus_get_header_length(ctx);
    uint8_t fc = req[hdr];

    if (fc == 0x06) {
        int addr = (req[hdr + 1] << 8) | req[hdr + 2];
        int val  = (req[hdr + 3] << 8) | req[hdr + 4];
        db_log_write(db, addr, (uint16_t)val);
    } else if (fc == 0x10) {
        int addr = (req[hdr + 1] << 8) | req[hdr + 2];
        int qty  = (req[hdr + 3] << 8) | req[hdr + 4];
        mb_regmap_lock();
        for (int i = 0; i < qty; i++) {
            uint16_t v = mb_regmap_get_holding(addr + i);
            db_log_write(db, addr + i, v);
        }
        mb_regmap_unlock();
    }
}

/**
 * @brief Modbus TCP slave thread.
 *
 * Opens a listening socket, then serves any number of connecting
 * masters (select()-multiplexed) from the shared register map until
 * the process's running flag is cleared.
 *
 * @param arg Pointer to an mb_thread_arg_t structure.
 */
void *mb_thread_func1(void *arg)
{
    extern volatile int running;

    mb_thread_arg_t *targ = (mb_thread_arg_t *)arg;

    slave_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    strncpy(ctx.bind_ip,
            (targ && targ->bind_ip[0]) ? targ->bind_ip : "0.0.0.0",
            sizeof(ctx.bind_ip) - 1);
    ctx.listen_port = (targ && targ->listen_port > 0) ? targ->listen_port : MODBUS_DEFAULT_PORT;
    ctx.slave_id    = (targ && targ->slave_id   > 0) ? targ->slave_id   : MODBUS_DEFAULT_SLAVE_ID;

    printf("[ MODBUS_TCP ] modbus tcp slave start\n");
    printf("[ MODBUS_TCP ] Bind IP    : %s\n", ctx.bind_ip);
    printf("[ MODBUS_TCP ] Port       : %d\n", ctx.listen_port);
    printf("[ MODBUS_TCP ] Slave ID   : %d\n", ctx.slave_id);

    if (db_init(&ctx.db) != 0) {
        fprintf(stderr, "[ MODBUS_TCP ] Thread exiting: DB init failed.\n");
        return NULL;
    }

    ctx.mb_ctx = modbus_new_tcp(ctx.bind_ip, ctx.listen_port);
    if (!ctx.mb_ctx) {
        fprintf(stderr, "[MB  ] Unable to allocate Modbus context: %s\n",
                modbus_strerror(errno));
        sqlite3_close(ctx.db);
        return NULL;
    }
    modbus_set_slave(ctx.mb_ctx, ctx.slave_id);

    modbus_mapping_t *mapping = build_shared_mapping();
    if (!mapping) {
        fprintf(stderr, "[MB  ] Unable to build register mapping\n");
        modbus_free(ctx.mb_ctx);
        sqlite3_close(ctx.db);
        return NULL;
    }

    int server_socket = modbus_tcp_listen(ctx.mb_ctx, MODBUS_TCP_MAX_CLIENTS);
    if (server_socket == -1) {
        fprintf(stderr, "[MB  ] modbus_tcp_listen failed: %s\n",
                modbus_strerror(errno));
        free_shared_mapping(mapping);
        modbus_free(ctx.mb_ctx);
        sqlite3_close(ctx.db);
        return NULL;
    }

    printf("[ MODBUS_TCP ] Listening on %s:%d (slave id=%d)\n",
           ctx.bind_ip, ctx.listen_port, ctx.slave_id);

    fd_set refset;
    FD_ZERO(&refset);
    FD_SET(server_socket, &refset);
    int fdmax = server_socket;

    while (running) {
        fd_set rdset = refset;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int rc = select(fdmax + 1, &rdset, NULL, NULL, &tv);
        if (rc == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[MB  ] select() failed: %s\n", strerror(errno));
            break;
        }
        if (rc == 0) continue; /* timeout — re-check running */

        for (int fd = 0; fd <= fdmax; fd++) {
            if (!FD_ISSET(fd, &rdset)) continue;

            if (fd == server_socket) {
                /* New incoming connection. */
                struct sockaddr_in caddr;
                socklen_t alen = sizeof(caddr);
                int newfd = accept(server_socket, (struct sockaddr *)&caddr, &alen);
                if (newfd < 0) {
                    perror("accept");
                    continue;
                }
                FD_SET(newfd, &refset);
                if (newfd > fdmax) fdmax = newfd;
                printf("[ MODBUS_TCP ] Master connected (fd=%d)\n", newfd);
            } else {
                /* Existing client sent a request (or disconnected). */
                modbus_set_socket(ctx.mb_ctx, fd);

                uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
                int qlen = modbus_receive(ctx.mb_ctx, query);

                if (qlen > 0) {
                    log_write_if_any(ctx.db, query, qlen, ctx.mb_ctx);
                    modbus_reply(ctx.mb_ctx, query, qlen, mapping);
                } else if (qlen == -1) {
                    printf("[ MODBUS_TCP ] Master disconnected (fd=%d)\n", fd);
                    close(fd);
                    FD_CLR(fd, &refset);
                    if (fd == fdmax) {
                        while (fdmax >= 0 && !FD_ISSET(fdmax, &refset)) fdmax--;
                    }
                }
            }
        }
    }

    printf("[ MODBUS_TCP ] Shutting down…\n");

    for (int fd = 0; fd <= fdmax; fd++) {
        if (fd != server_socket && FD_ISSET(fd, &refset)) close(fd);
    }
    close(server_socket);

    free_shared_mapping(mapping);
    modbus_free(ctx.mb_ctx);
    sqlite3_close(ctx.db);

    printf("[ MODBUS_TCP ] Stopped.\n");
    return NULL;
}
