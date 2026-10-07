/**
 * @file ota_logic.c
 * @brief OTA decisions without MQTT, libcurl or OpenSSL (see ota_logic.h).
 *
 * Moved out of ota.c unchanged in behaviour so it can be host-tested.
 */

#include "ota_logic.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/** @brief Status names, indexed by ota_status_t. */
static const char *status_str[] = {
    "STARTED",
    "DOWNLOADING",
    "VERIFYING",
    "APPLYING",
    "SUCCEEDED",
    "FAILED"
};

/** @brief Type names, indexed by ota_type_t. */
static const char *type_str[] = {
    "app",
    "system"
};

/* Documented in ota_logic.h. */
const char *ota_type_name(ota_type_t type)
{
    return type_str[type];
}

/* Documented in ota_logic.h. */
const char *ota_status_name(ota_status_t status)
{
    return status_str[status];
}

/* Documented in ota_logic.h. */
ota_parse_result_t ota_parse_request(const char *topic, const char *payload,
                                     const char *app_topic, const char *system_topic,
                                     ota_request_t *req)
{
    if (!topic || !payload || payload[0] == '\0')
        return OTA_REQ_NOT_OTA;

    ota_type_t type;

    if (strcmp(topic, app_topic) == 0)
        type = OTA_TYPE_APP;
    else if (strcmp(topic, system_topic) == 0)
        type = OTA_TYPE_SYSTEM;
    else
        return OTA_REQ_NOT_OTA;

    memset(req, 0, sizeof(*req));
    req->type = type;

    /* Read one byte more than the field holds to detect a URL that doesn't fit. */
    char url[sizeof(req->url) + 1];
    url[0] = '\0';
    json_get_string(payload, "url", url, sizeof(url));
    json_get_string(payload, "version", req->version, sizeof(req->version));

    if (url[0] == '\0')
        return OTA_REQ_MISSING_URL;

    if (strlen(url) >= sizeof(req->url))
        return OTA_REQ_URL_TOO_LONG;

    memcpy(req->url, url, strlen(url) + 1);

    if (strncasecmp(req->url, "https://", 8) != 0)
        return OTA_REQ_BAD_URL;

    if (type == OTA_TYPE_APP)
    {
        char sha[sizeof(req->sha256) + 1] = "";
        json_get_string(payload, "sha256", sha, sizeof(sha));

        size_t n = strlen(sha);
        if (n != 64 || strspn(sha, "0123456789abcdefABCDEF") != 64)
            return OTA_REQ_BAD_SHA256;

        memcpy(req->sha256, sha, 65);
    }

    return OTA_REQ_OK;
}

/* Documented in ota_logic.h. */
const char *ota_parse_error(ota_parse_result_t result)
{
    switch (result)
    {
    case OTA_REQ_BAD_URL:      return "invalid_url";
    case OTA_REQ_URL_TOO_LONG: return "url_too_long";
    case OTA_REQ_BAD_SHA256:   return "invalid_sha256";
    default:                   return NULL;
    }
}

/* Documented in ota_logic.h. */
int ota_status_json(char *buf, size_t len, ota_type_t type, ota_status_t status,
                    const char *version, const char *previous_version,
                    const char *error, long long timestamp)
{
    return snprintf(buf, len,
                    "{\"type\":\"%s\",\"status\":\"%s\","
                    "\"version\":\"%s\",\"previous_version\":\"%s\","
                    "\"error\":\"%s\",\"timestamp\":%lld}",
                    type_str[type],
                    status_str[status],
                    version ? version : "",
                    previous_version,
                    error ? error : "",
                    timestamp);
}

/* Documented in ota_logic.h. */
int ota_digest_matches(const unsigned char digest[32], const char *expected_hex, char actual_hex[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(actual_hex + i * 2, 3, "%02x", digest[i]);
    actual_hex[64] = '\0';

    return strcasecmp(actual_hex, expected_hex) == 0;
}

/* Documented in ota_logic.h. */
int ota_apply_app(const ota_app_ops_t *ops, const ota_request_t *req,
                  const char *download_dir, const char *binary_path)
{
    char dl_path[512];
    snprintf(dl_path, sizeof(dl_path), "%s/main.new", download_dir);

    if (req->sha256[0] == '\0')
    {
        ops->report(ops->ctx, OTA_STATUS_FAILED, req->version, "sha256_missing");
        return 0;
    }

    /* ---- Download ---- */
    ops->report(ops->ctx, OTA_STATUS_DOWNLOADING, req->version, "");

    if (!ops->download(ops->ctx, req->url, dl_path, 300))
    {
        ops->report(ops->ctx, OTA_STATUS_FAILED, req->version, "download_failed");
        return 0;
    }

    /* ---- Verify SHA256 (mandatory: never install an unverified binary) ---- */
    ops->report(ops->ctx, OTA_STATUS_VERIFYING, req->version, "");

    if (!ops->verify(ops->ctx, dl_path, req->sha256))
    {
        ops->unlink(ops->ctx, dl_path);
        ops->report(ops->ctx, OTA_STATUS_FAILED, req->version, "sha256_mismatch");
        return 0;
    }

    /* ---- Apply ---- */
    ops->report(ops->ctx, OTA_STATUS_APPLYING, req->version, "");

    /* Backup current binary. */
    char bak_path[512];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", binary_path);

    int err = ops->rename(ops->ctx, binary_path, bak_path);
    if (err != 0 && err != ENOENT)
    {
        fprintf(stderr, "[OTA] Backup failed: %s\n", strerror(err));
        ops->unlink(ops->ctx, dl_path);
        ops->report(ops->ctx, OTA_STATUS_FAILED, req->version, "backup_failed");
        return 0;
    }

    /* Move new binary into place. */
    err = ops->rename(ops->ctx, dl_path, binary_path);
    if (err != 0)
    {
        fprintf(stderr, "[OTA] Replace failed: %s\n", strerror(err));
        /* Restore backup. */
        ops->rename(ops->ctx, bak_path, binary_path);
        ops->report(ops->ctx, OTA_STATUS_FAILED, req->version, "replace_failed");
        return 0;
    }

    /* Set executable permission. */
    ops->make_executable(ops->ctx, binary_path);

    ops->report(ops->ctx, OTA_STATUS_SUCCEEDED, req->version, "");
    return 1;
}
