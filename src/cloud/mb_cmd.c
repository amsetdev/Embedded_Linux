/**
 * @file mb_cmd.c
 * @brief Modbus write commands received over MQTT: parse, check against the
 *        configuration, execute through the caller's ops, answer (see mb_cmd.h).
 */

#include "mb_cmd.h"
#include "json.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Longest requestId / method echoed in the response. */
#define ID_MAX 64

/** @brief A parsed command. */
typedef struct
{
    char method[ID_MAX + 1];              /**< "mb_write_single" or "mb_write_multiple" */
    int  multiple;                        /**< 0 = mb_write_single, 1 = mb_write_multiple */
    long slave;                           /**< Slave address 1..247 */
    long fc;                              /**< Function code 5, 6, 15 or 16 */
    long addr;                            /**< First register / coil address */
    long count;                           /**< Number of words / coils (1 for single) */
    long values[MB_CMD_MAX_VALUES];       /**< Words (0..65535) or coil states (0/1) */
} cmd_t;

/*-----------------------------------------------------------*/
/* Strict JSON number access                                  */
/*-----------------------------------------------------------*/

/**
 * @brief Finds the value of a key.
 * @param obj JSON text.
 * @param key Key name.
 * @return Position of the value (after ':' and whitespace), or NULL if absent.
 */
static const char *find_value(const char *obj, const char *key)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    for (const char *p = strstr(obj, pattern); p; p = strstr(p + 1, pattern))
    {
        const char *v = p + strlen(pattern);
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        if (*v != ':')
            continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        return v;
    }
    return NULL;
}

/**
 * @brief Parses one JSON integer (no fraction, exponent or quotes) and advances past it.
 * @param p   In/out: position of the number.
 * @param out Output: the value.
 * @return 0 on success, -1 if it is not an integer that fits a long.
 */
static int parse_int(const char **p, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(*p, &end, 10);
    if (end == *p || errno == ERANGE)
        return -1;
    if (*end == '.' || *end == 'e' || *end == 'E')
        return -1;
    if (*end && !strchr(" \t\r\n,}]", *end))
        return -1;
    *out = v;
    *p = end;
    return 0;
}

/**
 * @brief Reads an integer member.
 * @param obj JSON object text.
 * @param key Key name.
 * @param out Output: the value.
 * @return 0 = found, -1 = missing, -2 = present but not an integer.
 */
static int get_int(const char *obj, const char *key, long *out)
{
    const char *v = find_value(obj, key);
    if (!v)
        return -1;
    return parse_int(&v, out) == 0 ? 0 : -2;
}

/**
 * @brief Reads the "values" array of integers.
 * @param obj    JSON object text.
 * @param values Output array.
 * @param max    Size of values.
 * @return Number of integers, or -1 if missing, malformed or longer than max.
 */
static int get_values(const char *obj, long *values, int max)
{
    const char *p = find_value(obj, "values");
    if (!p || *p != '[')
        return -1;
    p++;
    int n = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == ']')
        return 0;
    for (;;)
    {
        if (n == max)
            return -1;
        if (parse_int(&p, &values[n]) != 0)
            return -1;
        n++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == ']')
            return n;
        if (*p != ',')
            return -1;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
    }
}

/*-----------------------------------------------------------*/
/* Response                                                   */
/*-----------------------------------------------------------*/

/**
 * @brief Appends a string as escaped JSON string content (truncated if out is full).
 * @param out  NUL-terminated buffer to append to.
 * @param size Size of out.
 * @param s    String to append.
 */
static void append_escaped(char *out, size_t size, const char *s)
{
    size_t n = strlen(out);
    for (; *s && n + 7 < size; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            out[n++] = '\\';
            out[n++] = (char)c;
        }
        else if (c < 0x20)
            n += (size_t)snprintf(out + n, size - n, "\\u%04x", c);
        else
            out[n++] = (char)c;
    }
    out[n] = '\0';
}

/**
 * @brief Builds the response JSON.
 * @param resp       Output buffer.
 * @param size       Size of resp.
 * @param request_id requestId of the command ("" = leave out).
 * @param method     method of the command ("" = leave out).
 * @param detail     NULL for "ok", else the error detail.
 */
static void build_response(char *resp, size_t size, const char *request_id,
                           const char *method, const char *detail)
{
    resp[0] = '\0';
    strncat(resp, "{", size - 1);
    if (request_id[0])
    {
        strncat(resp, "\"requestId\":\"", size - strlen(resp) - 1);
        append_escaped(resp, size, request_id);
        strncat(resp, "\",", size - strlen(resp) - 1);
    }
    if (method[0])
    {
        strncat(resp, "\"method\":\"", size - strlen(resp) - 1);
        append_escaped(resp, size, method);
        strncat(resp, "\",", size - strlen(resp) - 1);
    }
    if (!detail)
    {
        strncat(resp, "\"status\":\"ok\"}", size - strlen(resp) - 1);
        return;
    }
    strncat(resp, "\"status\":\"error\",\"detail\":\"", size - strlen(resp) - 1);
    append_escaped(resp, size, detail);
    strncat(resp, "\"}", size - strlen(resp) - 1);
}

/*-----------------------------------------------------------*/
/* Parsing                                                    */
/*-----------------------------------------------------------*/

/**
 * @brief Parses and range-checks the command (method already filled in).
 * @param json    NUL-terminated payload.
 * @param c       In/out: the command.
 * @param err     Output: the reason on error.
 * @param err_len Size of err.
 * @return 0 on success, -1 on error.
 */
static int parse_command(const char *json, cmd_t *c, char *err, size_t err_len)
{
    const char *p = json;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    const char *end = (*p == '{') ? json_value_end(p) : NULL;
    if (!end)
    {
        snprintf(err, err_len, "not a JSON object");
        return -1;
    }
    for (end++; *end; end++)
        if (!strchr(" \t\r\n", *end))
        {
            snprintf(err, err_len, "not a JSON object");
            return -1;
        }

    if (!c->method[0])
    {
        snprintf(err, err_len, "missing method");
        return -1;
    }
    if (strcmp(c->method, "mb_write_single") == 0)
        c->multiple = 0;
    else if (strcmp(c->method, "mb_write_multiple") == 0)
        c->multiple = 1;
    else
    {
        snprintf(err, err_len, "unknown method");
        return -1;
    }

    char *params = json_object_dup(json, "params");
    if (!params)
    {
        snprintf(err, err_len, "missing params");
        return -1;
    }

    int rc = -1;
    const char *names[] = { "slave", "fc", "addr", c->multiple ? "count" : "value" };
    long *dst[] = { &c->slave, &c->fc, &c->addr, c->multiple ? &c->count : &c->values[0] };
    for (int i = 0; i < 4; i++)
    {
        int r = get_int(params, names[i], dst[i]);
        if (r != 0)
        {
            snprintf(err, err_len, "%s params.%s", r == -1 ? "missing" : "not an integer:", names[i]);
            goto out;
        }
    }
    if (!c->multiple)
        c->count = 1;

    if (c->slave < 1 || c->slave > 247)
    {
        snprintf(err, err_len, "slave must be 1..247");
        goto out;
    }
    if (!c->multiple && c->fc != 5 && c->fc != 6)
    {
        snprintf(err, err_len, "mb_write_single needs fc 5 or 6");
        goto out;
    }
    if (c->multiple && c->fc != 15 && c->fc != 16)
    {
        snprintf(err, err_len, "mb_write_multiple needs fc 15 or 16");
        goto out;
    }
    if (c->count < 1 || c->count > MB_CMD_MAX_VALUES)
    {
        snprintf(err, err_len, "count must be 1..%d", MB_CMD_MAX_VALUES);
        goto out;
    }
    if (c->addr < 0 || c->addr + c->count - 1 > 65535)
    {
        snprintf(err, err_len, "addr out of range 0..65535");
        goto out;
    }
    if (c->multiple)
    {
        int n = get_values(params, c->values, MB_CMD_MAX_VALUES);
        if (n < 0)
        {
            snprintf(err, err_len, "params.values must be an array of up to %d integers",
                     MB_CMD_MAX_VALUES);
            goto out;
        }
        if (n != c->count)
        {
            snprintf(err, err_len, "count is %ld but values has %d", c->count, n);
            goto out;
        }
    }
    long max = (c->fc == 5 || c->fc == 15) ? 1 : 65535;
    for (long i = 0; i < c->count; i++)
        if (c->values[i] < 0 || c->values[i] > max)
        {
            snprintf(err, err_len, "value %ld out of range 0..%ld", c->values[i], max);
            goto out;
        }
    rc = 0;
out:
    free(params);
    return rc;
}

/*-----------------------------------------------------------*/
/* Safety rule: only configured, writable registers            */
/*-----------------------------------------------------------*/

/**
 * @brief Number of registers a configured point occupies.
 * @param pt The point.
 * @return 2 for int32 / float32, else 1.
 */
static int words_of(const ModbusPoint *pt)
{
    return (pt->data_type == 'f' || pt->data_type == 'd') ? 2 : 1;
}

/**
 * @brief Name of a register table for messages.
 * @param t Register table.
 * @return "holding register", "input register", "coil" or "discrete input".
 */
static const char *table_name(RegType t)
{
    switch (t)
    {
    case REG_HOLDING:  return "holding register";
    case REG_INPUT:    return "input register";
    case REG_COIL:     return "coil";
    default:           return "discrete input";
    }
}

/**
 * @brief Finds the configured point that covers an address (a 32-bit point covers 2).
 * @param pts   Configured points.
 * @param n     Number of points.
 * @param slave Slave address.
 * @param t     Register table.
 * @param addr  Address.
 * @return The point, or NULL if none.
 */
static const ModbusPoint *covering(const ModbusPoint *pts, int n, long slave, RegType t, long addr)
{
    for (int i = 0; i < n; i++)
    {
        const ModbusPoint *p = &pts[i];
        int words = (t == REG_HOLDING || t == REG_INPUT) ? words_of(p) : 1;
        if (p->slave_id == slave && p->reg_type == t &&
            addr >= p->address && addr < p->address + words)
            return p;
    }
    return NULL;
}

/**
 * @brief Explains why an address is not writable as table t.
 * @param pts     Configured points.
 * @param n       Number of points.
 * @param slave   Slave address.
 * @param t       Table the command writes.
 * @param addr    Address.
 * @param err     Output: the explanation.
 * @param err_len Size of err.
 */
static void not_configured(const ModbusPoint *pts, int n, long slave, RegType t, long addr,
                           char *err, size_t err_len)
{
    const RegType others[] = { REG_INPUT, REG_DISCRETE, REG_HOLDING, REG_COIL };
    for (int i = 0; i < 4; i++)
    {
        if (others[i] == t)
            continue;
        const ModbusPoint *p = covering(pts, n, slave, others[i], addr);
        if (p)
        {
            snprintf(err, err_len, "slave %ld addr %ld is %s, a%s %s, not a %s",
                     slave, addr, p->label,
                     (others[i] == REG_INPUT) ? "n" : "", table_name(others[i]), table_name(t));
            return;
        }
    }
    snprintf(err, err_len, "slave %ld addr %ld is not a configured %s", slave, addr, table_name(t));
}

/**
 * @brief The safety rule: every register the command writes is configured and writable.
 * @param c       The command.
 * @param pts     Configured points.
 * @param n       Number of points.
 * @param err     Output: the reason on refusal.
 * @param err_len Size of err.
 * @return 0 if allowed, -1 if refused.
 */
static int check_allowed(const cmd_t *c, const ModbusPoint *pts, int n, char *err, size_t err_len)
{
    RegType t = (c->fc == 5 || c->fc == 15) ? REG_COIL : REG_HOLDING;
    long last = c->addr + c->count - 1;

    for (long a = c->addr; a <= last;)
    {
        const ModbusPoint *p = covering(pts, n, c->slave, t, a);
        if (!p)
        {
            not_configured(pts, n, c->slave, t, a, err, err_len);
            return -1;
        }
        if (t == REG_HOLDING && words_of(p) == 2)
        {
            if (c->fc == 6)
            {
                snprintf(err, err_len, "%s is %s (2 registers): write both words with fc 16",
                         p->label, p->data_type == 'f' ? "float32" : "int32");
                return -1;
            }
            if (p->address != a || p->address + 1 > last)
            {
                snprintf(err, err_len, "%s (addr %d..%d) must be written as a whole",
                         p->label, p->address, p->address + 1);
                return -1;
            }
            a += 2;
        }
        else
            a++;
    }
    return 0;
}

/*-----------------------------------------------------------*/

mbc_result_t mb_cmd_handle(const mbc_ops_t *ops, const char *payload, size_t len,
                           int retained, char *resp, size_t resp_len)
{
    char request_id[ID_MAX + 1] = "";
    char err[160] = "";
    cmd_t c;
    memset(&c, 0, sizeof(c));

    if (!payload || len == 0 || len > MB_CMD_MAX_PAYLOAD || memchr(payload, '\0', len))
    {
        build_response(resp, resp_len, "", "", "size");
        return MBC_INVALID;
    }
    char *json = malloc(len + 1);
    if (!json)
    {
        build_response(resp, resp_len, "", "", "out of memory");
        return MBC_INVALID;
    }
    memcpy(json, payload, len);
    json[len] = '\0';

    if (json_get_string(json, "requestId", request_id, sizeof(request_id)) != 0)
        request_id[0] = '\0';
    if (json_get_string(json, "method", c.method, sizeof(c.method)) != 0)
        c.method[0] = '\0';

    mbc_result_t result = MBC_INVALID;
    if (retained)
    {
        /* A retained command would be written again on every reconnect. */
        build_response(resp, resp_len, request_id, c.method, "retained commands are not executed");
        goto out;
    }
    if (parse_command(json, &c, err, sizeof(err)) != 0)
    {
        build_response(resp, resp_len, request_id, c.method, err);
        goto out;
    }

    uint16_t words[MB_CMD_MAX_VALUES];
    for (long i = 0; i < c.count; i++)
        words[i] = (uint16_t)c.values[i];
    RegType t = (c.fc == 5 || c.fc == 15) ? REG_COIL : REG_HOLDING;

    ops->lock(ops->ctx);
    int n = 0;
    const ModbusPoint *pts = ops->points(ops->ctx, &n);
    if (check_allowed(&c, pts, n, err, sizeof(err)) != 0)
        result = MBC_REFUSED;
    else if (ops->write(ops->ctx, (int)c.slave, t, (uint16_t)c.addr, (int)c.count, words))
        result = MBC_OK;
    else
    {
        result = MBC_BUS_ERROR;
        snprintf(err, sizeof(err), "slave %ld did not confirm the write (timeout or exception)", c.slave);
    }
    ops->unlock(ops->ctx);

    build_response(resp, resp_len, request_id, c.method, result == MBC_OK ? NULL : err);
out:
    free(json);
    return result;
}
