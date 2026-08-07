#ifndef OTA_INSTALL_H
#define OTA_INSTALL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** Backup the currently running executable/version before install. */
int ota_backup_current(void);

/** Extract the downloaded package over the current installation. */
int ota_install_package(const char *package_path);

/** Restore the previous executable/version from backup (rollback). */
int ota_restore_backup(void);

/** Restart only the gateway application (systemctl restart gateway.service). */
int ota_restart_service(void);

/** Reboot the whole STM32MP1 board (used when config.reboot_after_update=true). */
int ota_reboot_board(void);

/**
 * Post-update health check. Waits briefly for gateway.service to come
 * back up and confirms it is active.
 * @retval true  service is healthy
 * @retval false service failed to (re)start -> caller should roll back
 */
bool ota_health_check(void);

#ifdef __cplusplus
}
#endif

#endif
