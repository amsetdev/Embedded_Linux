/**
 * @file mb_cmd.c
 * @brief Modbus write commands via MQTT.
 *
 * Parses JSON command messages received on the command topic and
 * dispatches single or block writes to the fieldbus driver.
 *
 * Command format (single write — FC05/FC06):
 * {
 *   "method": "mb_write_single",
 *   "params": { "slave": 1, "fc": 5, "addr": 100, "value": 65280 },
 *   "requestId": "optional-uuid"
 * }
 *
 * Command format (block write — FC15/FC16):
 * {
 *   "method": "mb_write_multiple",
 *   "params": { "slave": 3, "fc": 16, "addr": 300, "count": 2,
 *               "values": [1, 500] },
 *   "requestId": "optional-uuid"
 * }
 */

#include "mb_cmd.h"
#include "settings.h"
#include "data.h"
#include "json.h"
#include "mqtt.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief Maximum number of values in a block write command. */
#define MB_CMD_MAX_VALUES 125

/**
 * @brief Map a Modbus function code to a fieldbus register type.
 *
 * @param fc Modbus function code (5, 6, 15, or 16).
 * @param out Pointer to store the resulting fb_reg_type_t.
 *
 * @return 0 on success, -1 for unsupported function codes.
 */
static int fc_to_reg_type(int fc, fb_reg_type_t *out)
{
    switch (fc)
    {
    case 5:  /* Write Single Coil */
    case 15: /* Write Multiple Coils */
        *out = FB_REG_COIL;
        return 0;

    case 6:  /* Write Single Register */
    case 16: /* Write Multiple Registers */
        *out = FB_REG_HOLDING;
        return 0;

    default:
        return -1;
    }
}

/**
 * @brief Publish a JSON response for a command with a requestId.
 *
 * @param request_id The request identifier from the command.
 * @param method     The method that was invoked.
 * @param success    1 if the command succeeded, 0 otherwise.
 * @param detail     Optional detail string (may be NULL).
 */
static void publish_response(const char *request_id,
                             const char *method,
                             int success,
                             const char *detail)
{
    char buf[512];

    if (detail && detail[0])
    {
        snprintf(buf, sizeof(buf),
                 "{\"requestId\":\"%s\",\"method\":\"%s\","
                 "\"status\":\"%s\",\"detail\":\"%s\"}",
                 request_id,
                 method,
                 success ? "ok" : "error",
                 detail);
    }
    else
    {
        snprintf(buf, sizeof(buf),
                 "{\"requestId\":\"%s\",\"method\":\"%s\","
                 "\"status\":\"%s\"}",
                 request_id,
                 method,
                 success ? "ok" : "error");
    }

    mqtt_publish_to(cfg.cmd_response_topic, buf, 1);
}

/* -------------------------------------------------------------------------- */
/* Write handlers                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Handle a single-register/coil write command.
 *
 * Parses slave, fc, addr, value from the params object.
 */
static int handle_write_single(const char *params)
{
    int slave = 0, fc = 0, addr = 0, value = 0;

    if (json_get_int(params, "slave", &slave) != 0 ||
        json_get_int(params, "fc",    &fc)    != 0 ||
        json_get_int(params, "addr",  &addr)  != 0 ||
        json_get_int(params, "value", &value) != 0)
    {
        fprintf(stderr, "[MB_CMD] Missing params in mb_write_single\n");
        return 0;
    }

    fb_reg_type_t rt;

    if (fc_to_reg_type(fc, &rt) != 0)
    {
        fprintf(stderr, "[MB_CMD] Unsupported FC %d for single write\n", fc);
        return 0;
    }

    printf("[MB_CMD] Write single: slave=%d fc=%d addr=%d val=%d\n",
           slave, fc, addr, value);

    return data_write_register(slave, rt, (uint16_t)addr, (uint16_t)value);
}

/**
 * @brief Handle a block write command.
 *
 * Parses slave, fc, addr, count, values[] from the params object.
 */
static int handle_write_multiple(const char *params)
{
    int slave = 0, fc = 0, addr = 0, count = 0;

    if (json_get_int(params, "slave", &slave) != 0 ||
        json_get_int(params, "fc",    &fc)    != 0 ||
        json_get_int(params, "addr",  &addr)  != 0 ||
        json_get_int(params, "count", &count) != 0)
    {
        fprintf(stderr, "[MB_CMD] Missing params in mb_write_multiple\n");
        return 0;
    }

    if (count <= 0 || count > MB_CMD_MAX_VALUES)
    {
        fprintf(stderr, "[MB_CMD] Invalid count %d\n", count);
        return 0;
    }

    fb_reg_type_t rt;

    if (fc_to_reg_type(fc, &rt) != 0)
    {
        fprintf(stderr, "[MB_CMD] Unsupported FC %d for block write\n", fc);
        return 0;
    }

    int int_vals[MB_CMD_MAX_VALUES];
    int parsed = json_get_int_array(params, "values",
                                    int_vals, MB_CMD_MAX_VALUES);

    if (parsed < count)
    {
        fprintf(stderr,
                "[MB_CMD] values array: need %d, got %d\n",
                count, parsed);
        return 0;
    }

    /* Convert int array to uint16_t array. */
    uint16_t u16_vals[MB_CMD_MAX_VALUES];

    for (int i = 0; i < count; i++)
        u16_vals[i] = (uint16_t)int_vals[i];

    printf("[MB_CMD] Write multiple: slave=%d fc=%d addr=%d count=%d\n",
           slave, fc, addr, count);

    return data_write_block(slave, rt, (uint16_t)addr, count, u16_vals);
}

/* -------------------------------------------------------------------------- */
/* MQTT message callback                                                      */
/* -------------------------------------------------------------------------- */

void mb_cmd_on_message(struct mosquitto *m,
                       void *ud,
                       const struct mosquitto_message *msg)
{
    (void)m;
    (void)ud;

    if (!msg || !msg->topic || !msg->payload || msg->payloadlen == 0)
        return;

    /* Only handle messages on the command topic. */
    if (strcmp(msg->topic, cfg.cmd_topic) != 0)
        return;

    const char *json = (const char *)msg->payload;

    printf("[MB_CMD] Received command on %s\n", msg->topic);

    /* Extract method. */
    char method[64] = {0};

    if (json_get_string(json, "method", method, sizeof(method)) != 0)
    {
        fprintf(stderr, "[MB_CMD] No 'method' in payload\n");
        return;
    }

    /* Extract optional requestId. */
    char request_id[128] = {0};
    int has_request_id = (json_get_string(json, "requestId",
                                          request_id,
                                          sizeof(request_id)) == 0 &&
                          request_id[0] != '\0');

    /* Find the "params" object. */
    const char *params = strstr(json, "\"params\"");

    if (!params)
    {
        fprintf(stderr, "[MB_CMD] No 'params' in payload\n");

        if (has_request_id)
            publish_response(request_id, method, 0, "missing params");

        return;
    }

    /* Dispatch based on method. */
    int ok = 0;

    if (strcmp(method, "mb_write_single") == 0)
    {
        ok = handle_write_single(params);
    }
    else if (strcmp(method, "mb_write_multiple") == 0)
    {
        ok = handle_write_multiple(params);
    }
    else
    {
        fprintf(stderr, "[MB_CMD] Unknown method: %s\n", method);

        if (has_request_id)
            publish_response(request_id, method, 0, "unknown method");

        return;
    }

    /* Publish response if requestId was present. */
    if (has_request_id)
        publish_response(request_id, method, ok, NULL);
}
