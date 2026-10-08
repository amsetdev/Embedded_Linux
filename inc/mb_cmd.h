/**
 * @file mb_cmd.h
 * @brief Modbus write commands received over MQTT (no MQTT or libmodbus dependency, host-tested).
 *
 * The cloud publishes on MB_CMD_TOPIC_FMT (format of the feat/mb_write branch):
 * @code
 * {"requestId":"42","method":"mb_write_single",
 *  "params":{"slave":1,"fc":6,"addr":10,"value":1234}}
 * {"requestId":"43","method":"mb_write_multiple",
 *  "params":{"slave":1,"fc":16,"addr":20,"count":2,"values":[65534,7616]}}
 * @endcode
 * and every command is answered on MB_CMD_RESPONSE_TOPIC_FMT with
 * {"requestId":"42","method":"…","status":"ok"} or
 * {"requestId":"42","method":"…","status":"error","detail":"…"}
 * ("requestId" is left out when the command had none).
 *
 * Safety rule: a write is sent on the bus only if every register it writes is in
 * the loaded configuration (smart_rtu_config.json):
 *  - FC05 / FC15: coils of that slave; values 0 or 1.
 *  - FC06: a 16-bit holding register of that slave; value 0..65535.
 *  - FC16: holding registers of that slave covering every written word; a 32-bit
 *    register (int32, float32) only as a whole, high word first; words 0..65535.
 * Input registers and discrete inputs are read-only. Anything else is refused
 * and nothing is sent on the bus.
 */

#ifndef MB_CMD_H
#define MB_CMD_H

#include <stddef.h>
#include <stdint.h>

#include "data.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Topic the cloud publishes write commands on (%s = device_id). */
#define MB_CMD_TOPIC_FMT          "devices/%s/commands"

/** @brief Topic the firmware answers on (%s = device_id). */
#define MB_CMD_RESPONSE_TOPIC_FMT "devices/%s/commands/response"

/** @brief Largest accepted command in bytes. */
#define MB_CMD_MAX_PAYLOAD 8192

/** @brief Most words / coils in one mb_write_multiple (FC16 limit: 123). */
#define MB_CMD_MAX_VALUES 123

/** @brief Size of the response buffer callers should provide. */
#define MB_CMD_RESPONSE_MAX 512

/** @brief Side effects, provided by the caller. */
typedef struct
{
    void *ctx;   /**< Passed to every callback. */
    /** Takes the bus: no poll read or configuration reload until unlock(). */
    void (*lock)(void *ctx);
    /** Releases the bus. */
    void (*unlock)(void *ctx);
    /** The loaded registers (valid while the bus is locked). */
    const ModbusPoint *(*points)(void *ctx, int *count);
    /** Sends the write on the bus (bus locked); returns 1 if the slave confirmed it. */
    int  (*write)(void *ctx, int slave, RegType table, uint16_t addr,
                  int count, const uint16_t *values);
} mbc_ops_t;

/** @brief What mb_cmd_handle() did. */
typedef enum
{
    MBC_OK        = 0,   /**< Written and confirmed by the slave. */
    MBC_INVALID   = 1,   /**< Malformed command: nothing sent on the bus. */
    MBC_REFUSED   = 2,   /**< Not allowed by the configuration: nothing sent. */
    MBC_BUS_ERROR = 3    /**< Sent, but the slave did not confirm it. */
} mbc_result_t;

/**
 * @brief Checks and executes one write command and builds its response.
 *
 * @param ops      Side effects.
 * @param payload  Message payload (need not be NUL-terminated).
 * @param len      Payload length.
 * @param retained Non-zero for a retained message: answered with an error, never executed
 *                 (it would be written again on every reconnect).
 * @param resp     Output: the response JSON.
 * @param resp_len Size of resp (MB_CMD_RESPONSE_MAX is always enough).
 * @return What happened (mbc_result_t).
 */
mbc_result_t mb_cmd_handle(const mbc_ops_t *ops, const char *payload, size_t len,
                           int retained, char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif

#endif /* MB_CMD_H */
