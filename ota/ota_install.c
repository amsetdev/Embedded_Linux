/**
 * @file ota_install.c
 * @brief Backup / install / rollback / restart for the OTA service.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ota_install.h"
#include "ota_paths.h"

/*=============================================================
 * Private helper: run a shell command and report failures
 *============================================================*/
static int run_cmd(const char *cmd)
{
    int rc = system(cmd);

    if (rc == -1)
    {
        printf("OTA: system() failed to spawn shell for: %s\n", cmd);
        return -1;
    }

    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0)
    {
        printf("OTA: command exited with status %d: %s\n",
               WEXITSTATUS(rc), cmd);
        return -1;
    }

    if (!WIFEXITED(rc))
    {
        printf("OTA: command terminated abnormally: %s\n", cmd);
        return -1;
    }

    return 0;
}

int ota_backup_current(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && cp -f %s %s && cp -f %s %s",
             OTA_BACKUP_DIRECTORY,
             OTA_EXECUTABLE_PATH, OTA_BACKUP_EXECUTABLE_FILE,
             OTA_VERSION_FILE, OTA_BACKUP_VERSION_FILE);

    return run_cmd(cmd);
}

int ota_install_package(const char *package_path)
{
    char cmd[512];

    snprintf(cmd,
             sizeof(cmd),
             "tar -xzf %s -C ./ && chmod +x %s",
             package_path,
             OTA_EXECUTABLE_PATH);

    return run_cmd(cmd);
}

int ota_restore_backup(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd),
             "cp -f %s %s && cp -f %s %s",
             OTA_BACKUP_EXECUTABLE_FILE, OTA_EXECUTABLE_PATH,
             OTA_BACKUP_VERSION_FILE, OTA_VERSION_FILE);

    return run_cmd(cmd);
}

int ota_restart_service(void)
{
    return run_cmd("systemctl restart gateway.service");
}

int ota_reboot_board(void)
{
    /* Flush filesystem buffers before a hard reboot on the
     * STM32MP1 target -- avoids corrupting the eMMC/SD rootfs. */
    sync();

    return run_cmd("systemctl reboot");
}

bool ota_health_check(void)
{
    for (int attempt = 0; attempt < 15; attempt++)
    {
        sleep(2);

        int rc = system(
            "systemctl is-active --quiet gateway.service"
        );

        if (rc == 0)
        {
            printf("OTA: Health check passed\n");
            return true;
        }

        printf("OTA: Waiting for gateway.service (%d/15)\n",
               attempt + 1);
    }

    printf("OTA: Health check FAILED\n");
    return false;
}