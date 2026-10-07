/**
 * @file config_push.c
 * @brief Configuration pushed over MQTT (see config_push.h).
 */

#include "config_push.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Counts the objects in the "registers" array of a configuration.
 *
 * @param json NUL-terminated configuration.
 * @return Number of register objects, or -1 if there is no "registers" array.
 */
static int count_registers(const char *json)
{
    const char *p = strstr(json, "\"registers\"");

    if (!p)
        return -1;

    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '[')
        return -1;

    const char *end = json_value_end(p);
    if (!end)
        return -1;

    int n = 0;

    for (p++; p < end; p++)
    {
        if (*p == '{')
        {
            const char *obj_end = json_value_end(p);
            if (!obj_end || obj_end > end)
                return -1;
            n++;
            p = obj_end;
        }
    }

    return n;
}

/**
 * @brief Writes a rejection / error ack.
 *
 * @param ack     Output buffer.
 * @param ack_len Size of ack.
 * @param status  "rejected" or "error".
 * @param error   Error code.
 */
static void ack_error(char *ack, size_t ack_len, const char *status, const char *error)
{
    snprintf(ack, ack_len, "{\"status\":\"%s\",\"error\":\"%s\"}", status, error);
}

/* Documented in config_push.h. */
cp_result_t config_push_apply(const cp_ops_t *ops, const char *payload, size_t len,
                              char *ack, size_t ack_len)
{
    if (!payload || len == 0 || len > CONFIG_PUSH_MAX || memchr(payload, '\0', len))
    {
        ack_error(ack, ack_len, "rejected", "size");
        return CP_REJECTED;
    }

    char *json = malloc(len + 1);
    if (!json)
    {
        ack_error(ack, ack_len, "error", "memory");
        return CP_WRITE_FAILED;
    }
    memcpy(json, payload, len);
    json[len] = '\0';

    /* One complete object, nothing but whitespace after it. */
    const char *p = json;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    const char *end = json_value_end(p);
    int ok = *p == '{' && end != NULL;
    if (ok)
    {
        for (const char *q = end + 1; *q; q++)
            if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
                ok = 0;
    }

    char *device = ok ? json_object_dup(json, "device") : NULL;
    int has_device = device != NULL;
    int registers = ok ? count_registers(json) : -1;

    free(device);

    if (!ok || !has_device || registers < 0)
    {
        free(json);
        ack_error(ack, ack_len, "rejected", "not_a_config");
        return CP_REJECTED;
    }

    char *current = ops->read_current(ops->ctx);
    int same = current && strcmp(current, json) == 0;
    free(current);

    cp_result_t result;

    if (same)
    {
        result = CP_UNCHANGED;
        snprintf(ack, ack_len, "{\"status\":\"unchanged\",\"registers\":%d}", registers);
    }
    else if (ops->write_atomic(ops->ctx, json, len) != 0)
    {
        result = CP_WRITE_FAILED;
        ack_error(ack, ack_len, "error", "write_failed");
    }
    else
    {
        ops->request_reload(ops->ctx);
        result = CP_SAVED;
        snprintf(ack, ack_len, "{\"status\":\"saved\",\"registers\":%d}", registers);
    }

    free(json);
    return result;
}
