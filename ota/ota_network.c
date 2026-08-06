/**
 * @file ota_network.c
 * @brief OTA HTTPS download and verification.
 *
 * Responsibilities:
 * - Download latest.json
 * - Parse OTA metadata
 * - Download firmware package (with progress reporting)
 * - Download SHA256 checksum
 * - Verify package integrity
 */

#include "ota_network.h"
#include "ota_paths.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <openssl/evp.h>

/*=============================================================
 *                  Private Structures
 *============================================================*/

typedef struct
{
    FILE *fp;
    long downloaded;
    curl_off_t total;
    int last_reported_pct;
    bool report_progress;
} ota_download_t;

static size_t ota_write_callback(void *buffer, size_t size, size_t nmemb, void *userdata);
static int ota_xfer_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);
static int ota_download_file(const char *url, const char *filename, bool report_progress);

/*=============================================================
 *                 CURL Callbacks
 *============================================================*/

static size_t ota_write_callback(void *buffer, size_t size, size_t nmemb, void *userdata)
{
    ota_download_t *ctx = (ota_download_t *)userdata;
    size_t bytes = fwrite(buffer, size, nmemb, ctx->fp);

    ctx->downloaded += (long)(bytes * size);

    return bytes;
}

/* Called periodically by libcurl during a transfer; used to publish
 * download progress ("gateway/ota/progress") to ThingsBoard without
 * flooding the broker (reported at most every 10%). */
static int ota_xfer_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;

    ota_download_t *ctx = (ota_download_t *)clientp;

    if (!ctx->report_progress || dltotal <= 0)
    {
        return 0;
    }

    int pct = (int)((dlnow * 100) / dltotal);

    if (pct >= ctx->last_reported_pct + 10 || pct == 100)
    {
        ctx->last_reported_pct = pct;
        ota_publish_progress(pct);
    }

    return 0; /* returning non-zero would abort the transfer */
}

static int ota_download_file(const char *url, const char *filename, bool report_progress)
{
    CURL *curl;
    CURLcode ret;
    ota_download_t ctx;

    printf("Downloading:\n%s\n", url);

    memset(&ctx, 0, sizeof(ctx));
    ctx.report_progress = report_progress;
    ctx.fp = fopen(filename, "wb");

    if (ctx.fp == NULL)
    {
        printf("OTA: Cannot create %s\n", filename);
        return -1;
    }

    curl = curl_easy_init();

    if (curl == NULL)
    {
        fclose(ctx.fp);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ota_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    if (report_progress)
    {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ota_xfer_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    }

    ret = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    fclose(ctx.fp);

    if (ret != CURLE_OK)
    {
        printf("Download Failed : %s\n", curl_easy_strerror(ret));
        remove(filename);
        return -1;
    }

    printf("Downloaded %ld bytes\n", ctx.downloaded);

    return 0;
}

/*=============================================================
 *             Download latest.json
 *============================================================*/

int ota_download_latest_json(void)
{
    ota_context_t *ctx = ota_get_context();

    printf("\n=================================\n");
    printf("Downloading latest.json\n");
    printf("=================================\n");
    printf("URL : %s\n", ctx->config.latest_url);

    if (ota_download_file(ctx->config.latest_url, OTA_LATEST_JSON_FILE, false) != 0)
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

int ota_parse_latest_json(void)
{
    FILE *fp;
    long length;
    char *buffer;
    size_t nread;
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

    if (length <= 0)
    {
        printf("OTA: latest.json is empty\n");
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
        printf("OTA: Short read on latest.json\n");
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

    item = cJSON_GetObjectItem(root, "latest_version");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.latest_version, item->valuestring, OTA_VERSION_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "package_name");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.package_name, item->valuestring, OTA_FILENAME_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "package_url");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.package_url, item->valuestring, OTA_URL_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "sha256_url");
    if (cJSON_IsString(item))
    {
        strncpy(ctx->info.sha256_url, item->valuestring, OTA_URL_LEN - 1);
    }

    cJSON_Delete(root);

    printf("\n========== OTA Metadata ==========\n");
    printf("Current Version : %s\n", ctx->info.current_version);
    printf("Latest Version  : %s\n", ctx->info.latest_version);
    printf("Package Name    : %s\n", ctx->info.package_name);
    printf("Package URL     : %s\n", ctx->info.package_url);
    printf("SHA256 URL      : %s\n", ctx->info.sha256_url);
    printf("==================================\n");

    if (ota_compare_version(ctx->info.current_version, ctx->info.latest_version) == 1)
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

int ota_download_package(void)
{
    ota_context_t *ctx = ota_get_context();
    char package_path[OTA_PATH_LEN];

    snprintf(package_path, sizeof(package_path), "%s/%s",
             OTA_UPDATE_DIRECTORY, ctx->info.package_name);

    snprintf(ctx->info.package_path, sizeof(ctx->info.package_path), "%s", package_path);

    printf("\n=================================\n");
    printf("Downloading Firmware Package\n");
    printf("=================================\n");
    printf("URL : %s\n", ctx->info.package_url);
    printf("File: %s\n", package_path);

    ota_publish_status("DOWNLOADING");

    if (ota_download_file(ctx->info.package_url, package_path, true) != 0)
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

int ota_download_sha256(void)
{
    ota_context_t *ctx = ota_get_context();
    char sha_path[OTA_PATH_LEN];
    FILE *fp;

    snprintf(sha_path, sizeof(sha_path), "%s/package.sha256", OTA_UPDATE_DIRECTORY);

    printf("\n=================================\n");
    printf("Downloading SHA256\n");
    printf("=================================\n");
    printf("URL : %s\n", ctx->info.sha256_url);
    printf("File: %s\n", sha_path);

    if (ota_download_file(ctx->info.sha256_url, sha_path, false) != 0)
    {
        printf("OTA: SHA256 download failed\n");
        return -1;
    }

    fp = fopen(sha_path, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open SHA256 file\n");
        return -1;
    }

    if (fgets(ctx->info.sha256, sizeof(ctx->info.sha256), fp) == NULL)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    ctx->info.sha256[strcspn(ctx->info.sha256, "\r\n")] = '\0';

    printf("Expected SHA256:\n%s\n", ctx->info.sha256);

    return 0;
}

/*=============================================================
 *                 Verify Firmware Package
 *============================================================*/

int ota_verify_package(void)
{
    ota_context_t *ctx = ota_get_context();
    FILE *fp;
    EVP_MD_CTX *mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
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

    mdctx = EVP_MD_CTX_new();

    if (mdctx == NULL || EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1)
    {
        printf("OTA: Failed to initialize SHA256 context\n");
        fclose(fp);
        if (mdctx) EVP_MD_CTX_free(mdctx);
        return -1;
    }

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    {
        EVP_DigestUpdate(mdctx, buffer, bytes_read);
    }

    fclose(fp);

    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);

    for (i = 0; i < (int)hash_len && i < 32; i++)
    {
        sprintf(&calculated_hash[i * 2], "%02x", hash[i]);
    }

    calculated_hash[64] = '\0';

    printf("\n========== SHA256 ==========\n");
    printf("Expected : %s\n", ctx->info.sha256);
    printf("Actual   : %s\n", calculated_hash);
    printf("============================\n");

    if (strcasecmp(calculated_hash, ctx->info.sha256) != 0)
    {
        printf("OTA: SHA256 verification FAILED\n");
        return -1;
    }

    printf("OTA: SHA256 verification PASSED\n");
    return 0;
}
