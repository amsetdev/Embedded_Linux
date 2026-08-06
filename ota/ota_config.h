#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load ota_config.json into the OTA context (ota_get_context()->config).
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_load_config(void);

#ifdef __cplusplus
}
#endif

#endif
