/**
 * @file ota_network.c
 * @brief OTA HTTPS download and verification.
 *
 * This module implements all network related functionality
 * required by the OTA service.
 *
 * Responsibilities:
 * - Download latest.json
 * - Parse OTA metadata
 * - Download firmware package
 * - Download SHA256 file
 * - Verify package integrity
 * - Publish OTA status
 */

#include "ota_network.h"
#include "ota_paths.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include <cjson/cJSON.h>

#include <openssl/sha.h>

/*=============================================================
 *                  Private Structures
 *============================================================*/

/**
 * @brief CURL download context.
 */
typedef struct
{
    FILE *fp;

    long downloaded;

} ota_download_t;

/*=============================================================
 *              Private Function Prototypes
 *============================================================*/

static size_t ota_write_callback(void *buffer,
                                 size_t size,
                                 size_t nmemb,
                                 void *userdata);

static int ota_download_file(const char *url,
                             const char *filename);

/*=============================================================
 *                 CURL Write Callback
 *============================================================*/

/**
 * @brief CURL write callback.
 *
 * Stores downloaded bytes into a local file.
 *
 * @param buffer Received data.
 * @param size Element size.
 * @param nmemb Number of elements.
 * @param userdata File pointer.
 *
 * @return Number of bytes written.
 */
static size_t ota_write_callback(void *buffer,
                                 size_t size,
                                 size_t nmemb,
                                 void *userdata)
{
    ota_download_t *ctx;

    ctx = (ota_download_t *)userdata;

    size_t bytes;

    bytes = fwrite(buffer,
                   size,
                   nmemb,
                   ctx->fp);

    ctx->downloaded += (bytes * size);

    return bytes;
}

/*=============================================================
 *             Generic File Download
 *============================================================*/

/**
 * @brief Download file using HTTPS.
 *
 * @param url Download URL.
 * @param filename Destination filename.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
static int ota_download_file(const char *url,
                             const char *filename)
{
    CURL *curl;

    CURLcode ret;

    ota_download_t ctx;

    printf("Downloading:\n%s\n", url);

    ctx.downloaded = 0;

    ctx.fp = fopen(filename, "wb");

    if (ctx.fp == NULL)
    {
        printf("OTA: Cannot create %s\n",
               filename);

        return -1;
    }

    curl = curl_easy_init();

    if (curl == NULL)
    {
        fclose(ctx.fp);
        return -1;
    }

    curl_easy_setopt(curl,
                     CURLOPT_URL,
                     url);

    curl_easy_setopt(curl,
                     CURLOPT_FOLLOWLOCATION,
                     1L);

    curl_easy_setopt(curl,
                     CURLOPT_WRITEFUNCTION,
                     ota_write_callback);

    curl_easy_setopt(curl,
                     CURLOPT_WRITEDATA,
                     &ctx);

    curl_easy_setopt(curl,
                     CURLOPT_CONNECTTIMEOUT,
                     20L);

    curl_easy_setopt(curl,
                     CURLOPT_TIMEOUT,
                     300L);

    ret = curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    fclose(ctx.fp);

    if (ret != CURLE_OK)
    {
        printf("Download Failed : %s\n",
               curl_easy_strerror(ret));

        remove(filename);

        return -1;
    }

    printf("Downloaded %ld bytes\n",
           ctx.downloaded);

    return 0;
}

/*=============================================================
 *             Download latest.json
 *============================================================*/

/**
 * @brief Download latest.json from GitHub.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_latest_json(void)
{
    ota_context_t *ctx;

    ctx = ota_get_context();

    printf("\n=================================\n");
    printf("Downloading latest.json\n");
    printf("=================================\n");

    printf("URL : %s\n",
           ctx->config.latest_url);

    if (ota_download_file(ctx->config.latest_url,
                          OTA_LATEST_JSON_FILE) != 0)
    {
        printf("OTA: latest.json download failed\n");
        return -1;
    }

    printf("OTA: latest.json downloaded successfully\n");

    return 0;
}

/*=============================================================
 *             Parse latest.json
 *============================================================*/

/**
 * @brief Parse downloaded latest.json.
 *
 * Example JSON:
 *
 * {
 *   "latest_version":"1.1.0",
 *   "package_name":"gateway_v1.1.0.tar.gz",
 *   "package_url":"https://.....",
 *   "sha256_url":"https://....."
 * }
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_parse_latest_json(void)
{
    FILE *fp;

    long length;

    char *buffer;

    ota_context_t *ctx;

    cJSON *root = NULL;

    cJSON *item;

    ctx = ota_get_context();

    fp = fopen(OTA_LATEST_JSON_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Cannot open latest.json\n");
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

    item = cJSON_GetObjectItem(root,
                               "latest_version");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.latest_version,
                item->valuestring,
                OTA_VERSION_LEN - 1);
    }

    item = cJSON_GetObjectItem(root,
                               "package_name");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.package_name,
                item->valuestring,
                OTA_FILENAME_LEN - 1);
    }

    item = cJSON_GetObjectItem(root,
                               "package_url");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.package_url,
                item->valuestring,
                OTA_URL_LEN - 1);
    }

    item = cJSON_GetObjectItem(root,
                               "sha256_url");

    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.sha256_url,
                item->valuestring,
                OTA_URL_LEN - 1);
    }

    cJSON_Delete(root);

    printf("\n========== OTA Metadata ==========\n");

    printf("Current Version : %s\n",
           ctx->info.current_version);

    printf("Latest Version  : %s\n",
           ctx->info.latest_version);

    printf("Package Name    : %s\n",
           ctx->info.package_name);

    printf("Package URL     : %s\n",
           ctx->info.package_url);

    printf("SHA256 URL      : %s\n",
           ctx->info.sha256_url);

    printf("==================================\n");

    if (ota_compare_version(ctx->info.current_version,
                            ctx->info.latest_version) == 1)
    {
        ctx->info.update_available = true;

        printf("OTA: New firmware available\n");
    }
    else
    {
        ctx->info.update_available = false;

        printf("OTA: Already running latest firmware\n");
    }

    return 0;
}

/*=============================================================
 *             Download Firmware Package
 *============================================================*/

/**
 * @brief Download OTA firmware package.
 *
 * Downloads the firmware package from the URL
 * specified in latest.json.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_package(void)
{
    ota_context_t *ctx;

    char package_path[OTA_PATH_LEN];

    ctx = ota_get_context();

    snprintf(package_path,
             sizeof(package_path),
             "%s/%s",
             OTA_UPDATE_DIRECTORY,
             ctx->info.package_name);

    strncpy(ctx->info.package_path,
            package_path,
            sizeof(ctx->info.package_path) - 1);

    printf("\n=================================\n");
    printf("Downloading Firmware Package\n");
    printf("=================================\n");

    printf("URL : %s\n",
           ctx->info.package_url);

    printf("File: %s\n",
           package_path);

    ota_publish_status("DOWNLOADING");

    if (ota_download_file(ctx->info.package_url,
                          package_path) != 0)
    {
        printf("OTA: Firmware download failed\n");

        return -1;
    }

    printf("OTA: Firmware downloaded successfully\n");

    return 0;
}

/*=============================================================
 *             Download SHA256 File
 *============================================================*/

/**
 * @brief Download SHA256 checksum file.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_download_sha256(void)
{
    ota_context_t *ctx;

    char sha_path[OTA_PATH_LEN];

    ctx = ota_get_context();

    snprintf(sha_path,
             sizeof(sha_path),
             "%s/package.sha256",
             OTA_UPDATE_DIRECTORY);

    printf("\n=================================\n");
    printf("Downloading SHA256\n");
    printf("=================================\n");

    printf("URL : %s\n",
           ctx->info.sha256_url);

    printf("File: %s\n",
           sha_path);

    if (ota_download_file(ctx->info.sha256_url,
                          sha_path) != 0)
    {
        printf("OTA: SHA256 download failed\n");

        return -1;
    }

    FILE *fp;

    fp = fopen(sha_path, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open SHA256 file\n");
        return -1;
    }

    if (fgets(ctx->info.sha256,
              sizeof(ctx->info.sha256),
              fp) == NULL)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    ctx->info.sha256[
        strcspn(ctx->info.sha256, "\r\n")
    ] = '\0';

    printf("Expected SHA256:\n%s\n",
           ctx->info.sha256);

    return 0;
}

/*=============================================================
 *                 Verify Firmware Package
 *============================================================*/

/**
 * @brief Verify downloaded firmware package using SHA256.
 *
 * Calculates the SHA256 hash of the downloaded firmware
 * package and compares it with the checksum downloaded
 * from the OTA server.
 *
 * @retval 0 Verification successful
 * @retval -1 Verification failed
 */
int ota_verify_package(void)
{
    ota_context_t *ctx = ota_get_context();

    FILE *fp;

    SHA256_CTX sha_ctx;

    unsigned char hash[SHA256_DIGEST_LENGTH];

    unsigned char buffer[4096];

    size_t bytes_read;

    char calculated_hash[65];

    int i;

    fp = fopen(ctx->info.package_path, "rb");

    if (fp == NULL)
    {
        printf("OTA: Cannot open firmware package\n");
        return -1;
    }

    SHA256_Init(&sha_ctx);

    while ((bytes_read = fread(buffer,
                               1,
                               sizeof(buffer),
                               fp)) > 0)
    {
        SHA256_Update(&sha_ctx,
                      buffer,
                      bytes_read);
    }

    fclose(fp);

    SHA256_Final(hash,
                 &sha_ctx);

    for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        sprintf(&calculated_hash[i * 2],
                "%02x",
                hash[i]);
    }

    calculated_hash[64] = '\0';

    printf("\n========== SHA256 ==========\n");

    printf("Expected : %s\n",
           ctx->info.sha256);

    printf("Actual   : %s\n",
           calculated_hash);

    printf("============================\n");

    if (strcasecmp(calculated_hash,
                   ctx->info.sha256) != 0)
    {
        printf("OTA: SHA256 verification FAILED\n");
        return -1;
    }

    printf("OTA: SHA256 verification PASSED\n");

    return 0;
}

/*=============================================================
 *                 MQTT OTA Status
 *============================================================*/

/**
 * @brief Publish OTA state.
 */
int ota_publish_status(const char *status)
{
    char payload[128];

    snprintf(payload,
             sizeof(payload),
             "{"
             "\"state\":\"%s\""
             "}",
             status);

    return mqtt_publish("gateway/ota/status",
                        payload);
}

/**
 * @brief Publish OTA download progress.
 */
int ota_publish_progress(int percent)
{
    char payload[64];

    snprintf(payload,
             sizeof(payload),
             "{"
             "\"progress\":%d"
             "}",
             percent);

    return mqtt_publish("gateway/ota/progress",
                        payload);
}

/**
 * @brief Publish OTA result.
 */
int ota_publish_result(bool success)
{
    char payload[128];

    snprintf(payload,
             sizeof(payload),
             "{"
             "\"result\":\"%s\""
             "}",
             success ? "SUCCESS" : "FAILED");

    return mqtt_publish("gateway/ota/result",
                        payload);
}