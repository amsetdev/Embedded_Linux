/**
 * @file ota_logic.h
 * @brief OTA decisions without MQTT, libcurl or OpenSSL: request parsing,
 *        status message, digest check and the application-update sequence.
 *
 * Used by ota.c; compiled for the PC by the host tests (tests/host/test_ota_logic.c).
 * File system, download and hash access are passed in as function pointers
 * (ota_app_ops_t), so every branch can be tested without a board.
 */

#ifndef OTA_LOGIC_H
#define OTA_LOGIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief OTA update type. */
typedef enum
{
    OTA_TYPE_APP    = 0,    /**< Application binary update. */
    OTA_TYPE_SYSTEM = 1     /**< Full system image update.  */
} ota_type_t;

/** @brief OTA process status. */
typedef enum
{
    OTA_STATUS_STARTED     = 0,   /**< Request accepted. */
    OTA_STATUS_DOWNLOADING = 1,   /**< Download running. */
    OTA_STATUS_VERIFYING   = 2,   /**< Checking the SHA256 of the download. */
    OTA_STATUS_APPLYING    = 3,   /**< Installing. */
    OTA_STATUS_SUCCEEDED   = 4,   /**< Installed; restart/reboot follows. */
    OTA_STATUS_FAILED      = 5    /**< Aborted; the error field says why. */
} ota_status_t;

/** @brief OTA request descriptor. */
typedef struct
{
    ota_type_t type;            /**< Update type.                    */
    char       url[4096];       /**< Pre-signed download URL (https). */
    char       sha256[65];      /**< Expected SHA256 hex (app only). */
    char       version[32];     /**< Target version string.          */
    int        pending;         /**< 1 = request waiting to process. */
} ota_request_t;

/** @brief Result of ota_parse_request(). */
typedef enum
{
    OTA_REQ_OK           = 0,   /**< Valid request in *req. */
    OTA_REQ_NOT_OTA      = 1,   /**< Not an OTA topic, or no payload: ignore. */
    OTA_REQ_MISSING_URL  = 2,   /**< OTA topic, but no "url" in the payload. */
    OTA_REQ_BAD_URL      = 3,   /**< The URL is not https:// (status error "invalid_url"). */
    OTA_REQ_URL_TOO_LONG = 4,   /**< The URL does not fit ota_request_t.url ("url_too_long"). */
    OTA_REQ_BAD_SHA256   = 5    /**< App update without a 64-hex-digit "sha256" ("invalid_sha256"). */
} ota_parse_result_t;

/**
 * @brief Parses an OTA command message.
 *
 * The type comes from the topic (app_topic or system_topic). "url" and
 * "version" are read from the JSON payload (escapes decoded), "sha256" only
 * for app updates. The URL must start with https:// and fit the url field; an
 * app update must carry a 64-hex-digit sha256 (it is never installed
 * unverified). A too-long version is truncated.
 *
 * @param topic        MQTT topic of the message.
 * @param payload      NUL-terminated JSON payload.
 * @param app_topic    Configured application OTA topic.
 * @param system_topic Configured system OTA topic.
 * @param req          Output; zeroed and filled when the topic is an OTA topic.
 * @return OTA_REQ_OK, or why the request is ignored / rejected (ota_parse_result_t).
 */
ota_parse_result_t ota_parse_request(const char *topic, const char *payload,
                                     const char *app_topic, const char *system_topic,
                                     ota_request_t *req);

/**
 * @brief Name of an OTA type in status messages ("app", "system").
 *
 * @param type OTA type.
 * @return Static string.
 */
const char *ota_type_name(ota_type_t type);

/**
 * @brief Name of an OTA status in status messages ("STARTED" … "FAILED").
 *
 * @param status OTA status.
 * @return Static string.
 */
const char *ota_status_name(ota_status_t status);

/**
 * @brief Status-message error code for a rejected request.
 *
 * @param result ota_parse_request() result.
 * @return "invalid_url", "url_too_long", "invalid_sha256", or NULL for results
 *         that are not reported (OK, not an OTA topic, missing URL).
 */
const char *ota_parse_error(ota_parse_result_t result);

/**
 * @brief Formats the JSON status message published on the OTA status topic.
 *
 * {"type":…,"status":…,"version":…,"previous_version":…,"error":…,"timestamp":…}.
 * Strings are inserted as they are (not escaped).
 *
 * @param buf              Output buffer.
 * @param len              Size of buf.
 * @param type             Update type.
 * @param status           Status to report.
 * @param version          Target version (NULL = "").
 * @param previous_version Running version.
 * @param error            Error code (NULL = "").
 * @param timestamp        Unix time in seconds.
 * @return snprintf() result (length the message needs).
 */
int ota_status_json(char *buf, size_t len, ota_type_t type, ota_status_t status,
                    const char *version, const char *previous_version,
                    const char *error, long long timestamp);

/**
 * @brief Compares a SHA256 digest with an expected hex string (case-insensitive).
 *
 * @param digest       32-byte digest.
 * @param expected_hex Expected value, 64 hex characters.
 * @param actual_hex   Output: the digest as 64 lowercase hex characters + NUL (65 bytes).
 * @return 1 if equal, 0 otherwise.
 */
int ota_digest_matches(const unsigned char digest[32], const char *expected_hex, char actual_hex[65]);

/** @brief Side effects of an application update, provided by the caller. */
typedef struct
{
    void *ctx;   /**< Passed to every callback. */
    /** Downloads url to path; returns 1 on success. */
    int  (*download)(void *ctx, const char *url, const char *path, long timeout_sec);
    /** Checks the SHA256 of path against expected_hex; returns 1 if it matches. */
    int  (*verify)(void *ctx, const char *path, const char *expected_hex);
    /** rename(2); returns 0 or the errno value (ENOENT for a missing source). */
    int  (*rename)(void *ctx, const char *from, const char *to);
    /** unlink(2). */
    void (*unlink)(void *ctx, const char *path);
    /** chmod(path, 0755). */
    void (*make_executable)(void *ctx, const char *path);
    /** Publishes a status message (see ota_status_json()). */
    void (*report)(void *ctx, ota_status_t status, const char *version, const char *error);
} ota_app_ops_t;

/**
 * @brief Downloads, verifies and installs a new application binary.
 *
 * Sequence: DOWNLOADING → download to \<download_dir\>/main.new; VERIFYING →
 * SHA256 check (a request without sha256 fails with sha256_missing); APPLYING → rename the running
 * binary to \<binary\>.bak (a missing binary is fine), rename main.new into place
 * (on failure the backup is restored), make it executable; SUCCEEDED. Every failure
 * reports FAILED with sha256_missing, download_failed, sha256_mismatch,
 * backup_failed or replace_failed. Restarting the application is left to the caller.
 *
 * @param ops          Side effects.
 * @param req          The request (version, url, sha256).
 * @param download_dir Directory for the download.
 * @param binary_path  Path of the running application binary.
 * @return 1 if the new binary is installed (caller restarts), 0 on failure.
 */
int ota_apply_app(const ota_app_ops_t *ops, const ota_request_t *req,
                  const char *download_dir, const char *binary_path);

#ifdef __cplusplus
}
#endif

#endif /* OTA_LOGIC_H */
