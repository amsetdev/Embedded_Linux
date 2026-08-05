/**
 * @file ota_config.h
 * @brief OTA configuration loader.
 *
 * Reads the OTA configuration from a JSON file and fills
 * the OTA configuration structure.
 */

#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load OTA configuration.
 *
 * Reads ota_config.json and stores the values into the
 * OTA context.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_load_config(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CONFIG_H */