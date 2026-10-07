/**
 * @file payload.c
 * @brief Telemetry message builder (see payload.h).
 *
 * Moved out of mqtt.c (build_payload()) unchanged in behaviour so it can be
 * host-tested.
 */

#include "payload.h"

#include <math.h>
#include <stdio.h>

/* Documented in payload.h. */
int payload_build(char *buf, size_t buflen, long long ts_ms,
                  const ModbusPoint *points, int count)
{
    int pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts_ms);
    int first = 1;

    for (int i = 0; i < count && pos < (int)buflen - PAYLOAD_RESERVE; i++) {

        if (!points[i].valid)
            continue;

        if (!first)
            buf[pos++] = ',';

        switch (points[i].data_type)
        {
        case 'b':
            pos += snprintf(buf + pos,
                            buflen - pos,
                            "\"%s\":%s",
                            points[i].label,
                            points[i].value ? "true" : "false");
            break;

        case 'f':
            /* NaN/Inf have no JSON form: "nan" would make the whole message invalid. */
            if (isfinite(points[i].float_value))
                pos += snprintf(buf + pos,
                                buflen - pos,
                                "\"%s\":%.2f",
                                points[i].label,
                                points[i].float_value);
            else
                pos += snprintf(buf + pos,
                                buflen - pos,
                                "\"%s\":null",
                                points[i].label);
            break;

        default:
            pos += snprintf(buf + pos,
                            buflen - pos,
                            "\"%s\":%d",
                            points[i].label,
                            points[i].value);
            break;
        }

        first = 0;
    }

    pos += snprintf(buf + pos, buflen - pos, "}}");
    return pos;
}
