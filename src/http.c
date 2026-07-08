#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* Same CA bundle path your mqtt.c already uses for TLS in mosquitto_tls_set() */
#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

struct MemBuf { char *data; size_t len; };

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemBuf *mem = (struct MemBuf *)userp;

    char *ptr = realloc(mem->data, mem->len + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->len]), contents, realsize);
    mem->len += realsize;
    mem->data[mem->len] = 0;
    return realsize;
}

int http_init(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "[HTTP] curl_global_init failed\n");
        return 0;
    }
    printf("[ HTTP ] Module ready\n");
    return 1;
}

int http_post_json(const char *url, const char *json_payload,
                    char *resp_buf, size_t resp_buf_len)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct MemBuf chunk = { .data = malloc(1), .len = 0 };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    int ok = 0;

    if (res != CURLE_OK) {
        fprintf(stderr, "[HTTP] POST %s failed: %s\n", url, curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        ok = (http_code >= 200 && http_code < 300);
        printf("[ HTTP ] POST %s -> %ld (%s)\n", url, http_code, ok ? "OK" : "FAIL");
        if (resp_buf && resp_buf_len > 0) {
            strncpy(resp_buf, chunk.data, resp_buf_len - 1);
            resp_buf[resp_buf_len - 1] = '\0';
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(chunk.data);
    return ok;
}

int http_get(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct MemBuf chunk = { .data = malloc(1), .len = 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    int ok = 0;

    if (res != CURLE_OK) {
        fprintf(stderr, "[HTTP] GET %s failed: %s\n", url, curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        ok = (http_code >= 200 && http_code < 300);
        printf("[ HTTP ] GET %s -> %ld\n%s\n", url, http_code, chunk.data);
    }

    curl_easy_cleanup(curl);
    free(chunk.data);
    return ok;
}

void http_cleanup(void)
{
    curl_global_cleanup();
    printf("[ HTTP ] Cleaned up\n");
}