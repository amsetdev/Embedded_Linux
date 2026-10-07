/**
 * @file config_push.h
 * @brief Configuration pushed over MQTT (no MQTT or file system dependency, host-tested).
 *
 * The Smart RTU tool publishes the complete smart_rtu_config.json (retained,
 * QoS 1) on CONFIG_SET_TOPIC_FMT. The firmware checks it, saves it atomically
 * when it differs from the current file, asks for a configuration reload and
 * answers on CONFIG_ACK_TOPIC_FMT with
 * {"status":"saved"|"unchanged"|"rejected"|"error","registers":N,"error":"…"}.
 */

#ifndef CONFIG_PUSH_H
#define CONFIG_PUSH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Topic the tool publishes the configuration on (%s = device_id). */
#define CONFIG_SET_TOPIC_FMT "amset/%s/config/set"

/** @brief Topic the firmware answers on (%s = device_id). */
#define CONFIG_ACK_TOPIC_FMT "amset/%s/config/ack"

/** @brief Largest accepted configuration in bytes. */
#define CONFIG_PUSH_MAX (256 * 1024)

/** @brief Side effects, provided by the caller. */
typedef struct
{
    void *ctx;   /**< Passed to every callback. */
    /** Current configuration file content (malloc()ed, NUL-terminated) or NULL if none. */
    char *(*read_current)(void *ctx);
    /** Replaces the configuration file atomically (write temp + rename); returns 0 on success. */
    int   (*write_atomic)(void *ctx, const char *content, size_t len);
    /** Asks the application to reload settings and registers. */
    void  (*request_reload)(void *ctx);
} cp_ops_t;

/** @brief What config_push_apply() did. */
typedef enum
{
    CP_SAVED        = 0,   /**< New configuration saved, reload requested. */
    CP_UNCHANGED    = 1,   /**< Same as the current file: nothing written, no reload. */
    CP_REJECTED     = 2,   /**< Not a usable configuration: nothing written. */
    CP_WRITE_FAILED = 3    /**< Saving failed: the old configuration is still in place. */
} cp_result_t;

/**
 * @brief Checks and applies a pushed configuration and builds the ack message.
 *
 * Accepted: 1 to CONFIG_PUSH_MAX bytes without NUL, one complete JSON object
 * with a "device" object and a "registers" array. The ack carries the number of
 * register objects.
 *
 * @param ops     Side effects.
 * @param payload Message payload (need not be NUL-terminated).
 * @param len     Payload length.
 * @param ack     Output: the ack JSON.
 * @param ack_len Size of ack.
 * @return What happened (cp_result_t).
 */
cp_result_t config_push_apply(const cp_ops_t *ops, const char *payload, size_t len,
                              char *ack, size_t ack_len);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PUSH_H */
