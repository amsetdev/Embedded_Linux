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

int ota_load_config(void)
{
    ota_context_t *ctx;
    FILE *fp;
    long length;
    char *buffer;
    cJSON *root = NULL;
    cJSON *item;
    size_t nread;

    ctx = ota_get_context();

    fp = fopen(OTA_CONFIG_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open %s\n", OTA_CONFIG_FILE);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    rewind(fp);

    if (length <= 0)
    {
        printf("OTA: %s is empty\n", OTA_CONFIG_FILE);
        fclose(fp);
        return -1;
    }

    buffer = malloc((size_t)length + 1);

    if (buffer == NULL)
    {
        fclose(fp);
        return -1;
    }

    nread = fread(buffer, 1, (size_t)length, fp);
    fclose(fp);

    if (nread != (size_t)length)
    {
        printf("OTA: Short read on %s (%zu/%ld bytes)\n",
               OTA_CONFIG_FILE, nread, length);
        free(buffer);
        return -1;
    }

    buffer[length] = '\0';

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
        strncpy(ctx->config.latest_url, item->valuestring, OTA_URL_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "download_directory");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.download_directory, item->valuestring, OTA_PATH_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "backup_directory");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.backup_directory, item->valuestring, OTA_PATH_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "mqtt_host");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.mqtt_host, item->valuestring, sizeof(ctx->config.mqtt_host) - 1);
    }

    item = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(item))
    {
        ctx->config.mqtt_port = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "mqtt_topic");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->config.mqtt_topic, item->valuestring, sizeof(ctx->config.mqtt_topic) - 1);
    }

    item = cJSON_GetObjectItem(root, "reboot_after_update");
    if (cJSON_IsBool(item))
    {
        ctx->config.reboot_after_update = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);

    printf("\n========== OTA Configuration ==========\n");
    printf("Enabled            : %s\n", ctx->config.enabled ? "Yes" : "No");
    printf("Check Interval     : %d sec\n", ctx->config.check_interval);
    printf("Latest URL         : %s\n", ctx->config.latest_url);
    printf("Download Directory : %s\n", ctx->config.download_directory);
    printf("Backup Directory   : %s\n", ctx->config.backup_directory);
    printf("MQTT Host          : %s\n", ctx->config.mqtt_host);
    printf("MQTT Port          : %d\n", ctx->config.mqtt_port);
    printf("Reboot After Update: %s\n", ctx->config.reboot_after_update ? "Yes" : "No");
    printf("=======================================\n");

    return 0;
}
