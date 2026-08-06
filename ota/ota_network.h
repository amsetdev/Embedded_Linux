/**
 * @file ota_network.h
 * @brief OTA network interface (HTTPS download + SHA256 verify).
 */

#ifndef OTA_NETWORK_H
#define OTA_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

int ota_download_latest_json(void);

int ota_parse_latest_json(void);

int ota_download_package(void);

int ota_download_sha256(void);

int ota_verify_package(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_NETWORK_H */
