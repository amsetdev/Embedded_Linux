# Control Logic Implementation

This project can run control logic after each Modbus RTU polling cycle.

The RTU layer is now libmodbus-based. `src/control_logic.c` does not directly own a `modbus_t *`; instead it calls project wrapper functions:

```c
mb_write_single_coil()
mb_write_single_register()
```

Those wrappers are implemented in `src/modbus.c` using:

```c
modbus_write_bit()
modbus_write_register()
```

This keeps the control-logic code independent from the low-level RTU connection object.

## Where Control Logic Runs

Value-based control should run after:

```c
read_all_points();
```

At that point, the latest process values are available through:

```c
data_get_points()
```

`src/main.c` has this disabled-by-default hook:

```c
#if ENABLE_CONTROL_LOGIC
evaluate_value_logic_rules(value_rules, value_rule_count);
#endif
```

It is currently disabled:

```c
#define ENABLE_CONTROL_LOGIC 0
```

Change it to:

```c
#define ENABLE_CONTROL_LOGIC 1
```

only after the control rule addresses and output targets are verified.

## Example Value Rule

The example rule watches register address `100`, which matches:

```csv
Voltage_L1,100,Holding,w,V
```

Example:

```c
static value_logic_t value_rules[] = {
    {
        .pv_address  = 100,
        .pv_is_input = 0,
        .hh = 260.0f,
        .h  = 250.0f,
        .l  = 200.0f,
        .ll = 190.0f,
        .action_hh = ACTION_ON,
        .action_h  = ACTION_ON,
        .action_l  = ACTION_OFF,
        .action_ll = ACTION_OFF,
        .target_hh = { .type = TARGET_COIL, .address = 10 },
        .target_h  = { .type = TARGET_COIL, .address = 10 },
        .target_l  = { .type = TARGET_COIL, .address = 10 },
        .target_ll = { .type = TARGET_COIL, .address = 10 },
        .last_zone = 0,
    },
};
```

This means:

- Read PV from holding register `100`.
- If PV reaches `H` or `HH`, write coil `10` ON.
- If PV reaches `L` or `LL`, write coil `10` OFF.

The logic is edge-triggered. It writes only when the PV enters a different zone.

## Register Output Example

For holding-register output instead of coil output:

```c
.target_hh = {
    .type = TARGET_REGISTER,
    .address = 200,
    .high_value = 1,
    .low_value = 0,
}
```

With:

```c
.action_hh = ACTION_HIGH
```

the code writes `high_value` to holding register `200`.

With:

```c
.action_ll = ACTION_LOW
```

the code writes `low_value` to the target register.

## Time-Based Rules

Time rules can be evaluated with:

```c
evaluate_time_logic_rules(time_rules, time_rule_count);
```

A time rule writes one action at `start_time` and another at `end_time`.

Time strings can be parsed with:

```c
parse_schedule_time("08:07:2026 14:30:00", &rule.start_time);
parse_schedule_time("08:07:2026 18:30:00", &rule.end_time);
```

The expected format is:

```text
dd:mm:yyyy hh:mm:ss
```

## Safety Note

Do not enable `ENABLE_CONTROL_LOGIC` until these are verified:

1. PV address is correct.
2. Output coil/register address is correct.
3. ON/OFF or HIGH/LOW action is correct.
4. Slave ID in `settings.config` is correct.
5. The physical output is safe to switch from software.

