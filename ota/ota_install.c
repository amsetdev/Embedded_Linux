#include <stdio.h>
#include <stdlib.h>

#include "ota_install.h"
#include "ota_paths.h"

int ota_backup_current(void)
{
    char cmd[512];

    snprintf(cmd,
             sizeof(cmd),
             "mkdir -p %s && cp %s %s",
             OTA_BACKUP_DIRECTORY,
             OTA_EXECUTABLE_PATH,
             OTA_BACKUP_EXECUTABLE_FILE);

    return system(cmd);
}

int ota_install_package(void)
{
    char cmd[512];

    snprintf(cmd,
             sizeof(cmd),
             "tar -xzf %s -C ./",
             OTA_PACKAGE_FILE);

    return system(cmd);
}

int ota_restore_backup(void)
{
    char cmd[512];

    snprintf(cmd,
             sizeof(cmd),
             "cp %s %s",
             OTA_BACKUP_EXECUTABLE_FILE,
             OTA_EXECUTABLE_PATH);

    return system(cmd);
}

int ota_restart_service(void)
{
    return system("systemctl restart gateway.service");
}