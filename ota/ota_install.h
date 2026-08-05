#ifndef OTA_INSTALL_H
#define OTA_INSTALL_H

#ifdef __cplusplus
extern "C" {
#endif

int ota_backup_current(void);

int ota_install_package(void);

int ota_restore_backup(void);

int ota_restart_service(void);

#ifdef __cplusplus
}
#endif

#endif