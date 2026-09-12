# Modbus RTU Thread Explanation

This document explains the Modbus RTU thread created in `src/main.c`:

```c
pthread_create(&mb_thread_id, NULL, mb_thread_func, NULL);
```

The thread runs in the background. The main thread keeps the display and touch UI responsive, while this thread polls Modbus values, builds the MQTT payload, and publishes it.

## Current RTU Implementation


The RTU protocol layer is now libmodbus-based.

The project still keeps wrapper function names such as:

```c
uart_open()
mb_transaction()
mb_write_single_coil()
mb_write_single_register()
```

But internally `src/modbus.c` now uses libmodbus:

```c
modbus_new_rtu()
modbus_connect()
modbus_set_slave()
modbus_read_registers()
modbus_read_input_registers()
modbus_read_bits()
modbus_read_input_bits()
modbus_write_bit()
modbus_write_register()
```

So the application no longer manually builds Modbus RTU request frames, appends CRC bytes, writes raw bytes with `write()`, or reads raw response bytes with `select()` and `read()`.

libmodbus now handles:

- RTU frame creation
- CRC generation
- response reading
- response validation
- Modbus exception handling
- read/write function-code behavior

## RS485 Direction Control

The board still uses PE10 as the RS485 direction-control pin.

RS485 is half-duplex, so the transceiver must switch between:

- TX mode before sending a request
- RX mode before waiting for the slave reply

This is still handled by the project because PE10 is board-specific.

`src/modbus.c` registers a libmodbus RTS callback:

```c
modbus_rtu_set_custom_rts(rtu_ctx, libmodbus_set_rts);
modbus_rtu_set_rts(rtu_ctx, MODBUS_RTU_RTS_UP);
```

When libmodbus is about to transmit, it calls the callback with `on = 1`. The callback drives PE10 high.

After transmission, libmodbus calls the callback with `on = 0`. The callback drives PE10 low.

So the split is now:

```text
libmodbus:
    builds Modbus RTU frames
    calculates CRC
    sends request
    reads response
    validates response

project code:
    controls PE10 RS485 TX/RX direction
    maps existing wrapper functions to libmodbus calls
```

## Startup Flow

Before this thread starts, `main()` performs the important setup:

1. Loads settings from `settings.config`.
2. Initializes the RS485 direction-control GPIO.
3. Creates and connects the libmodbus RTU context using `uart_open()`.
4. Initializes the display and touch input.
5. Parses `registers.csv` using `parse_csv()`.
6. Initializes MQTT.
7. Starts the Modbus RTU background thread.

The thread is started with:

```c
pthread_create(&mb_thread_id, NULL, mb_thread_func, NULL);
```

Meaning:

- `&mb_thread_id`: stores the thread handle so `main()` can later wait for the thread using `pthread_join()`.
- `NULL`: uses default pthread attributes.
- `mb_thread_func`: function that runs inside the new thread.
- `NULL`: no argument is passed to the thread function.

## One Polling Cycle

Each cycle does this:

1. Calls `read_all_points()`.
2. Counts how many points were read successfully.
3. Updates `mb_cycle`, `mb_ok_flag`, and `mb_success_cnt`.
4. Builds the MQTT JSON payload.
5. Publishes the payload.
6. Sleeps for `cfg.interval` seconds.

The loop continues while:

```c
running
```

is non-zero.

## Reading registers.csv

The CSV is parsed by `parse_csv()` in `src/data.c`.

Example:

```csv
Label,Address,Register Type,Data Type,Unit
Voltage_L1,100,Holding,w,V
Voltage_L2,101,Holding,w,V
Voltage_L3,102,Holding,w,V
Current_L1,103,Holding,w,A
Current_L2,104,Holding,w,A
Current_L3,105,Holding,w,A
Frequency,106,Holding,w,Hz
Power_Factor_L1,107,Holding,w,
Power_Factor_L2,108,Holding,w,
Power_Factor_L3,109,Holding,w,
Active_Power_L1,110,Holding,w,W
Active_Power_L2,111,Holding,w,W
```

All rows above are `Holding`, so `read_point()` selects function code `0x03`.

`mb_transaction()` then maps that function code to:

```c
modbus_read_registers(rtu_ctx, addr, 1, &reg);
```

For the sample CSV, the thread reads:

- `Voltage_L1` from holding register `100`
- `Voltage_L2` from holding register `101`
- `Voltage_L3` from holding register `102`
- `Current_L1` from holding register `103`
- `Current_L2` from holding register `104`
- `Current_L3` from holding register `105`
- `Frequency` from holding register `106`
- `Power_Factor_L1` from holding register `107`
- `Power_Factor_L2` from holding register `108`
- `Power_Factor_L3` from holding register `109`
- `Active_Power_L1` from holding register `110`
- `Active_Power_L2` from holding register `111`

## Function Code Mapping

The wrapper `mb_transaction()` supports the existing project function-code model:

| Function code | Meaning | libmodbus call |
|---|---|---|
| `0x01` | Read coils | `modbus_read_bits()` |
| `0x02` | Read discrete inputs | `modbus_read_input_bits()` |
| `0x03` | Read holding registers | `modbus_read_registers()` |
| `0x04` | Read input registers | `modbus_read_input_registers()` |

Write helpers use:

| Operation | libmodbus call |
|---|---|
| Write single coil | `modbus_write_bit()` |
| Write single holding register | `modbus_write_register()` |

## Shared Status For Display

After reading all points, the thread updates shared status:

```c
pthread_mutex_lock(&points_mutex);
mb_ok_flag = (uart_fd >= 0);
mb_success_cnt = s;
mb_cycle++;
pthread_mutex_unlock(&points_mutex);
```

The main UI loop reads these values under the same mutex before drawing the status screen.

## MQTT Publish

After each read cycle:

```c
build_payload(payload, sizeof(payload));
mqtt_publish(payload);
```

Only valid points are included in the JSON payload.

## Shutdown

When `Ctrl+C` or `SIGTERM` is received:

```c
running = 0;
```

The Modbus thread exits its loop and returns. Then `main()` joins the thread and closes the libmodbus RTU connection with `uart_close()`.

## Important Code Note

There is still a possible thread-id bug in `src/main.c`.

At file scope:

```c
static pthread_t mb_thread_id;
```

Later, inside `main()`, another local variable with the same name is declared for the TCP thread:

```c
pthread_t mb_thread_id;
pthread_create(&mb_thread_id, NULL, mb_thread_func1, &mb_arg);
```

That local variable shadows the file-scope RTU thread id.

Recommended names:

```c
static pthread_t mb_rtu_thread_id;
static pthread_t mb_tcp_thread_id;
```

Then create and join them separately.

