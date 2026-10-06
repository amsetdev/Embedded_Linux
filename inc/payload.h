/**
 * @file payload.h
 * @brief Telemetry message builder (no MQTT dependency, host-tested).
 *
 * The message is the contract with the cloud:
 * {"ts":\<unix ms\>,"values":{"\<label\>":\<value\>,...}} with one entry per
 * register that was read successfully in this cycle.
 */

#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stddef.h>

#include "data.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes kept free at the end of the buffer: no value is added once fewer remain. */
#define PAYLOAD_RESERVE 128

/**
 * @brief Builds the telemetry JSON for a set of points.
 *
 * Points with valid == 0 (failed reads) are left out. Values by data_type:
 * 'f' as "%.2f" of float_value, 'b' as true/false, everything else as the
 * integer value. Labels are inserted as they are (not escaped). Once fewer
 * than PAYLOAD_RESERVE bytes are left, the remaining points are left out.
 *
 * @param buf    Output buffer.
 * @param buflen Size of buf (must be > PAYLOAD_RESERVE).
 * @param ts_ms  Timestamp in Unix milliseconds.
 * @param points Points to publish.
 * @param count  Number of points.
 * @return Length of the message in buf.
 */
int payload_build(char *buf, size_t buflen, long long ts_ms,
                  const ModbusPoint *points, int count);

#ifdef __cplusplus
}
#endif

#endif /* PAYLOAD_H */
