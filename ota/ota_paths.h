/**
 * @file ota_paths.h
 * @brief OTA file and directory path definitions.
 *
 * This file contains all filesystem paths used by the OTA
 * update system. Centralizing these paths makes it easier
 * to maintain and modify the OTA directory structure.
 *
 * @note This is the single source of truth for all OTA paths.
 *       No other header should redefine these macros.
 */

#ifndef OTA_PATHS_H
#define OTA_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 *                              OTA ROOT
 *============================================================================*/

/** @brief OTA root directory. */
#define OTA_ROOT_DIRECTORY            "./"

/*==============================================================================
 *                              CONFIGURATION
 *============================================================================*/

/** @brief OTA configuration file. */
#define OTA_CONFIG_FILE               OTA_ROOT_DIRECTORY "ota_config.json"

/** @brief Current firmware version file. */
#define OTA_VERSION_FILE              OTA_ROOT_DIRECTORY "version.txt"

/** @brief Path of the currently running executable (backed up/restored on update). */
#define OTA_EXECUTABLE_PATH           "./main"

/*==============================================================================
 *                              DIRECTORIES
 *============================================================================*/

/** @brief Directory for downloaded OTA files. */
#define OTA_UPDATE_DIRECTORY          OTA_ROOT_DIRECTORY "update"

/** @brief Directory for firmware backup. */
#define OTA_BACKUP_DIRECTORY          OTA_ROOT_DIRECTORY "backup"

/** @brief Directory for temporary OTA files. */
#define OTA_TEMP_DIRECTORY            OTA_ROOT_DIRECTORY "temp"

/*==============================================================================
 *                              OTA FILES
 *============================================================================*/

/** @brief Downloaded OTA manifest. */
#define OTA_LATEST_JSON_FILE          OTA_UPDATE_DIRECTORY "/latest.json"

/** @brief Downloaded firmware package. */
#define OTA_PACKAGE_FILE              OTA_UPDATE_DIRECTORY "/gateway.tar.gz"

/** @brief Downloaded SHA256 checksum file. */
#define OTA_SHA256_FILE               OTA_UPDATE_DIRECTORY "/gateway.sha256"

/** @brief Temporary firmware package. */
#define OTA_TEMP_PACKAGE_FILE         OTA_TEMP_DIRECTORY "/gateway.tar.gz"

/*==============================================================================
 *                              BACKUP FILES
 *============================================================================*/

/** @brief Backup firmware package. */
#define OTA_BACKUP_PACKAGE_FILE       OTA_BACKUP_DIRECTORY "/gateway_backup.tar.gz"

/** @brief Backup version file. */
#define OTA_BACKUP_VERSION_FILE       OTA_BACKUP_DIRECTORY "/version.txt"

/** @brief Backup of the executable. */
#define OTA_BACKUP_EXECUTABLE_FILE    OTA_BACKUP_DIRECTORY "/main"

#ifdef __cplusplus
}
#endif

#endif /* OTA_PATHS_H */