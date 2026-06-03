#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include "modbus_rtu.h"
#include "mqtt.h"
#include "config.h"

modbus_t *mb_ctx = NULL;

/* ============================================================================
 * CSV PARSER — loads register definitions from registers.csv
 * ========================================================================== */

static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static int split_csv(char *line, char *cols[], int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        cols[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p  = c + 1;
    }
    return n;
}

int parse_csv(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        LOG_WARN("'%s' not found — using 20 sample points", CONFIG_FILE);
        for (int i = 0; i < 20; i++) {
            snprintf(points[i].label, LABEL_MAX, "Point_%d", i + 1);
            points[i].address   = 400 + i;
            points[i].reg_type  = REG_HOLDING;
            points[i].data_type = 'w';
            points[i].unit[0]   = '\0';
            points[i].valid     = 0;
        }
        point_count = 20;
        return 1;
    }

    char line[512];
    int  first = 1, idx = 0;
    int  cl = -1, ca = -1, cr = -1, cu = -1;

    while (fgets(line, sizeof(line), f) && idx < MAX_POINTS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strlen(line)) continue;

        char *cols[16];
        int   nc = split_csv(line, cols, 16);

        if (first) {
            for (int c = 0; c < nc; c++) {
                char tmp[64];
                strncpy(tmp, cols[c], 63);
                for (int i = 0; tmp[i]; i++) tmp[i] = tolower((unsigned char)tmp[i]);
                char *s = str_trim(tmp);
                if      (!strcmp(s, "label"))        cl = c;
                else if (!strcmp(s, "address"))      ca = c;
                else if (strstr(s, "register"))      cr = c;
                else if (!strcmp(s, "unit"))         cu = c;
            }
            first = 0;
            if (cl < 0 || ca < 0) { fclose(f); return 0; }
            continue;
        }

        if (nc <= cl || nc <= ca) continue;
        char *lbl = str_trim(cols[cl]);
        if (!strlen(lbl)) continue;

        strncpy(points[idx].label, lbl, LABEL_MAX - 1);

        char ab[32];
        strncpy(ab, str_trim(cols[ca]), 31);
        char *sp = strchr(ab, ' ');
        if (sp) *sp = '\0';
        points[idx].address = atoi(ab);

        points[idx].reg_type = REG_HOLDING;
        if (cr >= 0 && cr < nc) {
            char rt[32];
            strncpy(rt, cols[cr], 31);
            for (int i = 0; rt[i]; i++) rt[i] = tolower((unsigned char)rt[i]);
            if      (strstr(rt, "coil"))     points[idx].reg_type = REG_COIL;
            else if (strstr(rt, "discrete")) points[idx].reg_type = REG_DISCRETE;
            else if (strstr(rt, "input"))    points[idx].reg_type = REG_INPUT;
        }

        points[idx].data_type = 'w';
        points[idx].unit[0]   = '\0';
        if (cu >= 0 && cu < nc)
            strncpy(points[idx].unit, str_trim(cols[cu]), UNIT_MAX - 1);

        points[idx].valid = 0;
        idx++;
    }

    fclose(f);
    point_count = idx;
    LOG_INFO("Loaded %d points from %s", point_count, CONFIG_FILE);
    return 1;
}

/* ============================================================================
 * MODBUS CONNECTION
 * ========================================================================== */

int mb_connect(void)
{
    LOG_INFO("Modbus: %s @ %d baud slave %d",
             cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);

    mb_ctx = modbus_new_rtu(cfg.modbus_port, cfg.modbus_baud,
                            MODBUS_PARITY, MODBUS_DATA_BITS, MODBUS_STOP_BITS);
    if (!mb_ctx) {
        LOG_ERROR("modbus_new_rtu: %s", modbus_strerror(errno));
        return 0;
    }

    modbus_set_slave(mb_ctx, cfg.modbus_slave);
    modbus_set_response_timeout(mb_ctx, 2, 0);

    if (modbus_connect(mb_ctx) == -1) {
        LOG_ERROR("modbus_connect: %s", modbus_strerror(errno));
        modbus_free(mb_ctx);
        mb_ctx = NULL;
        return 0;
    }

    LOG_INFO("Modbus connected");
    return 1;
}

void mb_disconnect(void)
{
    if (mb_ctx) {
        modbus_close(mb_ctx);
        modbus_free(mb_ctx);
        mb_ctx = NULL;
    }
}

/* ============================================================================
 * POINT READING
 * ========================================================================== */

int read_point(ModbusPoint *pt)
{
    if (!mb_ctx) return 0;

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        uint16_t reg = 0;
        uint8_t  bit = 0;
        int      rc  = -1;

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
            default:
                rc = modbus_read_registers(mb_ctx, pt->address, 1, &reg);
                if (rc == 1) { pt->value = reg; pt->valid = 1; return 1; }
                break;
        }
        usleep(100000);
    }

    pt->valid = 0;
    return 0;
}

void read_all_points(void)
{
    int s = 0, fail = 0;
    time_t start = time(NULL);

    for (int i = 0; i < point_count && running; i++) {
        if (read_point(&points[i])) s++; else fail++;
        usleep(POINT_DELAY_US);
    }

    LOG_INFO("READ DONE — ok:%d fail:%d time:%ds",
             s, fail, (int)(time(NULL) - start));
}

/* ============================================================================
 * JSON PAYLOAD BUILDER
 * ========================================================================== */

void build_payload(char *buf, size_t buflen)
{
    long long ts  = (long long)time(NULL) * 1000;
    int       pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts);
    int       first = 1;

    for (int i = 0; i < point_count && pos < (int)buflen - 128; i++) {
        if (!points[i].valid) continue;
        if (!first) buf[pos++] = ',';
        pos += snprintf(buf + pos, buflen - pos,
                        "\"%s\":%d", points[i].label, points[i].value);
        first = 0;
    }
    snprintf(buf + pos, buflen - pos, "}}");
}

/* ============================================================================
 * BACKGROUND THREAD
 * ========================================================================== */

void *mb_thread_func(void *arg)
{
    (void)arg;
    static char payload[PAYLOAD_MAX];

    while (running) {
        read_all_points();

        int s = 0;
        for (int i = 0; i < point_count; i++) if (points[i].valid) s++;

        pthread_mutex_lock(&points_mutex);
        mb_ok_flag     = (mb_ctx != NULL);
        mb_success_cnt = s;
        mb_cycle++;
        pthread_mutex_unlock(&points_mutex);

        LOG_INFO("MB cycle %d done — %d/%d ok", mb_cycle, s, point_count);

        build_payload(payload, sizeof(payload));
        mqtt_publish(payload);

        /* Sleep in small chunks so shutdown is responsive */
        for (int t = 0; t < cfg.interval * 10 && running; t++)
            usleep(100000);
    }

    return NULL;
}