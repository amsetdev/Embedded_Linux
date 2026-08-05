/**
 * @file ota_config.c
 * @brief OTA configuration loader.
 */

#include "ota_config.h"
#include "ota_paths.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

/*=============================================================
 *                  Public Functions
 *============================================================*/

/**
 * @brief Load OTA configuration.
 *
 * Reads the OTA configuration JSON file and fills
 * the OTA context.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_load_config(void)
{
    ota_context_t *ctx;

    FILE *fp;

    long length;

    char *buffer;

    cJSON *root = NULL;

    cJSON *item;

    ctx = ota_get_context();

    fp = fopen(OTA_CONFIG_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open %s\n",
               OTA_CONFIG_FILE);

        return -1;
    }

    fseek(fp, 0, SEEK_END);

    length = ftell(fp);

    rewind(fp);

    buffer = malloc(length + 1);

    if (buffer == NULL)
    {
        fclose(fp);
        return -1;
    }

    fread(buffer, 1, length, fp);

    buffer[length] = '\0';

    fclose(fp);

    root = cJSON_Parse(buffer);

    free(buffer);

    if (root == NULL)
    {
        printf("OTA: JSON Parse Error\n");
        return -1;
    }

        item = cJSON_GetObjectItem(root, "enabled");

    if (cJSON_IsBool(item))
    {
        ctx->config.enabled = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(root, "check_interval");

    if (cJSON_IsNumber(item))
    {
        ctx->config.check_interval = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "latest_url");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.latest_url,
                item->valuestring,
                OTA_URL_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "download_directory");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.download_directory,
                item->valuestring,
                OTA_PATH_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "mqtt_host");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.mqtt_host,
                item->valuestring,
                sizeof(ctx->config.mqtt_host) - 1);
    }

    item = cJSON_GetObjectItem(root, "mqtt_port");

    if (cJSON_IsNumber(item))
    {
        ctx->config.mqtt_port = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "mqtt_token");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.mqtt_token,
                item->valuestring,
                sizeof(ctx->config.mqtt_token) - 1);
    }
    printf("MQTT Host          : %s\n", ctx->config.mqtt_host);
    printf("MQTT Port          : %d\n", ctx->config.mqtt_port);
    printf("MQTT Token         : %s\n", ctx->config.mqtt_token);

    cJSON_Delete(root);

        printf("\n========== OTA Configuration ==========\n");

    printf("Enabled            : %s\n",
           ctx->config.enabled ? "Yes" : "No");

    printf("Check Interval     : %d sec\n",
           ctx->config.check_interval);

    printf("Latest URL         : %s\n",
           ctx->config.latest_url);

    printf("Download Directory : %s\n",
           ctx->config.download_directory);

    printf("Backup Directory   : %s\n",
           ctx->config.backup_directory);

    printf("=======================================\n");

    return 0;
}