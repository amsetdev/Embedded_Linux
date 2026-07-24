#include "data.h"
#include "modbus.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>


static ModbusPoint points[MAX_POINTS];
static int         point_count = 0;

ModbusPoint *data_get_points(void) { return points; }
int          data_get_count(void)  { return point_count; }


static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static int split_csv(char *line, char *cols[], int max)
{
    int n = 0; char *p = line;
    while (n < max) {
        cols[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0'; p = c+1;
    }
    return n;
}

int parse_csv(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        printf("[ DATA ] '%s' not found — using 20 sample points\n", CONFIG_FILE);
        for (int i = 0; i < 20; i++) {
            snprintf(points[i].label, LABEL_MAX, "Point_%d", i+1);
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
    int first=1, idx=0, cl=-1, ca=-1, cr=-1, cu=-1;
    while (fgets(line, sizeof(line), f) && idx < MAX_POINTS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strlen(line)) continue;
        char *cols[16]; int nc = split_csv(line, cols, 16);

        if (first) {
            for (int c=0; c<nc; c++) {
                char tmp[64]; strncpy(tmp, cols[c], 63);
                for (int i=0; tmp[i]; i++) tmp[i] = tolower((unsigned char)tmp[i]);
                char *s = str_trim(tmp);
                if      (!strcmp(s,"label"))        cl = c;
                else if (!strcmp(s,"address"))      ca = c;
                //else if (strstr(s,"register"))      cr = c;
               // else if (!strcmp(s,"unit"))         cu = c;
            }
            first = 0;
            if (cl < 0 || ca < 0) { fclose(f); return 0; }
            continue;
        }

        if (nc <= cl || nc <= ca) continue;
        char *lbl = str_trim(cols[cl]); if (!strlen(lbl)) continue;
        strncpy(points[idx].label, lbl, LABEL_MAX-1);

        char ab[32]; strncpy(ab, str_trim(cols[ca]), 31);
        char *sp = strchr(ab,' '); if (sp) *sp = '\0';
        points[idx].address  = atoi(ab);
        points[idx].reg_type = REG_HOLDING;

        if (cr >= 0 && cr < nc) {
            char rt[32]; strncpy(rt, cols[cr], 31);
            for (int i=0; rt[i]; i++) rt[i] = tolower((unsigned char)rt[i]);
            if      (strstr(rt,"coil"))     points[idx].reg_type = REG_COIL;
            else if (strstr(rt,"discrete")) points[idx].reg_type = REG_DISCRETE;
            else if (strstr(rt,"input"))    points[idx].reg_type = REG_INPUT;
        }

        points[idx].data_type = 'w';
        points[idx].unit[0]   = '\0';
        if (cu >= 0 && cu < nc)
            strncpy(points[idx].unit, str_trim(cols[cu]), UNIT_MAX-1);

        points[idx].valid = 0;
        idx++;
    }
    fclose(f);
    point_count = idx;
    printf("[ DATA ] Loaded %d points from %s\n", point_count, CONFIG_FILE);
    return 1;
}

int read_point(ModbusPoint *pt)
{
    uint8_t fc;
    switch (pt->reg_type) {
        case REG_COIL:     fc = 0x01; break;
        case REG_DISCRETE: fc = 0x02; break;
        case REG_INPUT:    fc = 0x04; break;
        default:           fc = 0x03; break;
    }

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        uint16_t val = 0;
        if (mb_transaction((uint8_t)cfg.modbus_slave, fc,
                           (uint16_t)pt->address, &val)) {
            pt->value = (int)val;
            pt->valid = 1;
            return 1;
        }
        printf("[ DATA ] Point '%s' addr %d retry %d\n",
               pt->label, pt->address, retry + 1);
        usleep(50000);
    }
    pt->valid = 0;
    return 0;
}

void read_all_points(void)
{
    extern volatile int running;
    int s = 0, f = 0;
    time_t start = time(NULL);
    for (int i = 0; i < point_count && running; i++) {
        if (read_point(&points[i])) s++; else f++;
        usleep(POINT_DELAY_US);
    }
    printf("[ DATA ] READ DONE — ok:%d fail:%d time:%ds\n",
           s, f, (int)(time(NULL) - start));
}

