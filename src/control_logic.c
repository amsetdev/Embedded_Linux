#include "control_logic.h"
#include <modbus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

int parse_schedule_time(const char *str, struct tm *out)
{
    /* Format from the spec: dd:mm:yyyy hh:mm:ss */
    memset(out, 0, sizeof(struct tm));
    int d, mo, y, h, mi, s;

    if (sscanf(str, "%d:%d:%d %d:%d:%d", &d, &mo, &y, &h, &mi, &s) != 6) {
        return -1;
    }

    out->tm_mday = d;
    out->tm_mon  = mo - 1;   /* struct tm months are 0-11 */
    out->tm_year = y - 1900;
    out->tm_hour = h;
    out->tm_min  = mi;
    out->tm_sec  = s;
    out->tm_isdst = -1;

    return 0;
}

/* Writes a single action to a target over Modbus.
 * modbus_ctx must be a valid, already-connected modbus_t*. */
static int write_action(modbus_t *ctx, target_t *tgt, action_t action)
{
    int rc;

    if (tgt->type == TARGET_COIL) {
        int coil_val = (action == ACTION_ON) ? 1 : 0;
        rc = modbus_write_bit(ctx, tgt->address, coil_val);
    } else { /* TARGET_REGISTER */
        uint16_t reg_val = (action == ACTION_HIGH) ? tgt->high_value
                                                     : tgt->low_value;
        rc = modbus_write_register(ctx, tgt->address, reg_val);
    }

    if (rc == -1) {
        fprintf(stderr, "Modbus write failed on addr %d: %s\n",
                tgt->address, modbus_strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * 1. Value-Based Control Logic
 * ------------------------------------------------------------------- */

/* Classify which zone the PV currently sits in.
 * -2 = at/below LL, -1 = at/below L (but above LL), 0 = normal band,
 *  1 = at/above H,   2 = at/above HH                                 */
static int classify_zone(value_logic_t *l, float pv)
{
    if (pv >= l->hh) return  2;
    if (pv >= l->h)  return  1;
    if (pv <= l->ll) return -2;
    if (pv <= l->l)  return -1;
    return 0;
}

int evaluate_value_logic(void *modbus_ctx, value_logic_t *logic, float pv_value)
{
    modbus_t *ctx = (modbus_t *)modbus_ctx;
    int zone = classify_zone(logic, pv_value);

    /* Edge-triggered: only act when the zone actually changes.
     * Delete this check if you want the action re-sent on every poll. */
    if (zone == logic->last_zone) {
        return 0;
    }

    int fired = 0;

    switch (zone) {
        case 2:  /* HH */
            fired = (write_action(ctx, &logic->target_hh, logic->action_hh) == 0);
            break;
        case 1:  /* H */
            fired = (write_action(ctx, &logic->target_h, logic->action_h) == 0);
            break;
        case -1: /* L */
            fired = (write_action(ctx, &logic->target_l, logic->action_l) == 0);
            break;
        case -2: /* LL */
            fired = (write_action(ctx, &logic->target_ll, logic->action_ll) == 0);
            break;
        default:
            /* back to normal band - no action defined by the spec here */
            break;
    }

    logic->last_zone = zone;
    return fired;
}

/* ---------------------------------------------------------------------
 * 2. Time-Based Control Logic
 * ------------------------------------------------------------------- */

int evaluate_time_logic(void *modbus_ctx, time_logic_t *logic)
{
    modbus_t *ctx = (modbus_t *)modbus_ctx;
    time_t now_t = time(NULL);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);

    time_t start_t = mktime(&logic->start_time);
    time_t end_t   = mktime(&logic->end_time);

    int fired = 0;

    if (!logic->start_fired && now_t >= start_t) {
        fired |= (write_action(ctx, &logic->target_start, logic->action_start) == 0);
        logic->start_fired = 1;
    }

    if (!logic->end_fired && now_t >= end_t) {
        fired |= (write_action(ctx, &logic->target_end, logic->action_end) == 0);
        logic->end_fired = 1;
    }

    return fired;
}
