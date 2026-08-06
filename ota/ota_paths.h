/**
 * @file ota_paths.h
 * @brief OTA file and directory path definitions.
 *
 * Centralized filesystem paths used by the OTA update system.
 * No other header should redefine these macros.
 */

#ifndef OTA_PATHS_H
#define OTA_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

/* OTA root directory */
#define OTA_ROOT_DIRECTORY            "./"

/* Configuration */
#define OTA_CONFIG_FILE               OTA_ROOT_DIRECTORY "ota_config.json"
#define OTA_VERSION_FILE              OTA_ROOT_DIRECTORY "version.txt"

/* Path of the currently running gateway executable (backed up/restored on update) */
#define OTA_EXECUTABLE_PATH           "./main"

/* Directories */
#define OTA_UPDATE_DIRECTORY          OTA_ROOT_DIRECTORY "update"
#define OTA_BACKUP_DIRECTORY          OTA_ROOT_DIRECTORY "backup"
#define OTA_TEMP_DIRECTORY            OTA_ROOT_DIRECTORY "temp"

/* Downloaded files */
#define OTA_LATEST_JSON_FILE          OTA_UPDATE_DIRECTORY "/latest.json"
#define OTA_PACKAGE_FILE              OTA_UPDATE_DIRECTORY "/gateway.tar.gz"
#define OTA_SHA256_FILE               OTA_UPDATE_DIRECTORY "/gateway.sha256"
#define OTA_TEMP_PACKAGE_FILE         OTA_TEMP_DIRECTORY "/gateway.tar.gz"

/* Backup files */
#define OTA_BACKUP_PACKAGE_FILE       OTA_BACKUP_DIRECTORY "/gateway_backup.tar.gz"
#define OTA_BACKUP_VERSION_FILE       OTA_BACKUP_DIRECTORY "/version.txt"
#define OTA_BACKUP_EXECUTABLE_FILE    OTA_BACKUP_DIRECTORY "/main"

#ifdef __cplusplus
}
#endif

#endif /* OTA_PATHS_H */
