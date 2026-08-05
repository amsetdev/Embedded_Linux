/**
 * @file ota_network.h
 * @brief OTA network interface.
 *
 * This module provides all HTTPS communication required by
 * the OTA service.
 *
 * Responsibilities:
 * - Download latest.json
 * - Parse OTA metadata
 * - Download firmware package
 * - Download SHA256 checksum
 * - Verify downloaded package
 * - Publish OTA status over MQTT
 */

#ifndef OTA_NETWORK_H
#define OTA_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/*=============================================================
 *                  Download Functions
 *============================================================*/

/**
 * @brief Download latest.json from GitHub.
 *
 * Downloads the OTA metadata file specified in
 * ota_config.json.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_latest_json(void);

/**
 * @brief Parse downloaded latest.json.
 *
 * Reads OTA metadata and fills the OTA context.
 *
 * Parsed fields:
 * - latest_version
 * - package_name
 * - package_url
 * - sha256_url
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_parse_latest_json(void);

/**
 * @brief Download firmware package.
 *
 * Downloads the firmware package from package_url.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_package(void);

/**
 * @brief Download SHA256 checksum file.
 *
 * Downloads the checksum corresponding to the
 * firmware package.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_sha256(void);

/*=============================================================
 *                 Verification Functions
 *============================================================*/

/**
 * @brief Verify firmware package integrity.
 *
 * Calculates the SHA256 checksum of the downloaded
 * package and compares it with the expected value.
 *
 * @retval 0 Verification successful
 * @retval -1 Verification failed
 */
int ota_verify_package(void);

/*=============================================================
 *                MQTT Status Functions
 *============================================================*/

/**
 * @brief Publish OTA state.
 *
 * Example:
 * {
 *   "ota_state":"DOWNLOADING"
 * }
 *
 * @param status Status string.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_publish_status(const char *status);

/**
 * @brief Publish download progress.
 *
 * Example:
 * {
 *   "progress":45
 * }
 *
 * @param percent Download percentage.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_publish_progress(int percent);

/**
 * @brief Publish OTA result.
 *
 * Example:
 * {
 *   "result":"SUCCESS"
 * }
 *
 * @param success true if update succeeded.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_publish_result(bool success);

#ifdef __cplusplus
}
#endif

#endif /* OTA_NETWORK_H */