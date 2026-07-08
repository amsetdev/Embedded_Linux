#ifndef CONTROL_LOGIC_H
#define CONTROL_LOGIC_H

#include <stdint.h>
#include <time.h>

/* ---------------------------------------------------------------------
 * Action types shared by both logic modes
 * ------------------------------------------------------------------- */
typedef enum {
    ACTION_OFF  = 0,
    ACTION_ON   = 1,
    ACTION_LOW  = 2,
    ACTION_HIGH = 3
} action_t;

/* How to write the action out over Modbus */
typedef enum {
    TARGET_COIL,      /* ON/OFF -> FC05 write_bit            */
    TARGET_REGISTER   /* HIGH/LOW -> FC06 write_register      */
} target_type_t;

typedef struct {
    target_type_t type;
    int           address;     /* coil addr or holding-register addr */
    /* Only used when type == TARGET_REGISTER and action is HIGH/LOW */
    uint16_t      high_value;
    uint16_t      low_value;
} target_t;

/* ---------------------------------------------------------------------
 * 1. Value-Based Control Logic
 * ------------------------------------------------------------------- */
typedef struct {
    int      pv_address;   /* holding/input register address of the PV */
    int      pv_is_input;  /* 1 = input register (FC04), 0 = holding (FC03) */

    float    hh, h, l, ll; /* setpoints, hh > h > l > ll */

    action_t action_hh;
    action_t action_h;
    action_t action_l;
    action_t action_ll;

    target_t target_hh;
    target_t target_h;
    target_t target_l;
    target_t target_ll;

    /* internal: last zone the PV was in, used to fire actions only
     * on transition instead of every poll (edge-triggered) */
    int      last_zone; /* -2=LL,-1=L,0=normal,1=H,2=HH */
} value_logic_t;

/* ---------------------------------------------------------------------
 * 2. Time-Based Control Logic
 * ------------------------------------------------------------------- */
typedef struct {
    struct tm start_time;
    struct tm end_time;

    action_t  action_start;
    action_t  action_end;

    target_t  target_start;
    target_t  target_end;

    /* internal: prevents re-firing every poll once triggered */
    int       start_fired;
    int       end_fired;
} time_logic_t;

/* ---------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------- */

/* Parses "dd:mm:yyyy hh:mm:ss" into struct tm. Returns 0 on success. */
int  parse_schedule_time(const char *str, struct tm *out);

/* Evaluates value-based logic against a freshly read PV value.
 * ctx is your modbus_t* connection handle (opaque here to avoid
 * pulling in modbus.h in the header). Returns 1 if an action fired. */
int  evaluate_value_logic(void *modbus_ctx, value_logic_t *logic, float pv_value);

/* Evaluates time-based logic against the current wall-clock time.
 * Returns 1 if an action fired. */
int  evaluate_time_logic(void *modbus_ctx, time_logic_t *logic);

#endif /* CONTROL_LOGIC_H */
