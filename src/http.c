#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

int http_init(void)
{
    return (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

void http_cleanup(void)
{
    curl_global_cleanup();
}

static int perform_request(CURL *curl)
{
    CURLcode res = curl_easy_perform(curl);

    if(res != CURLE_OK)
    {
        printf("CURL Error : %s\n", curl_easy_strerror(res));
        return 0;
    }

    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    printf("\nHTTP Status : %ld\n", code);

    return (code >= 200 && code < 300);
}

int http_get(const char *url)
{
    CURL *curl = curl_easy_init();

    if(!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);

    int ret = perform_request(curl);

    curl_easy_cleanup(curl);

    return ret;
}

int http_post(const char *url, const char *json)
{
    CURL *curl = curl_easy_init();

    if(!curl)
        return 0;

    struct curl_slist *headers = NULL;

    headers = curl_slist_append(headers,
                                "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);

    int ret = perform_request(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return ret;
}

int http_put(const char *url, const char *json)
{
    CURL *curl = curl_easy_init();

    if(!curl)
        return 0;

    struct curl_slist *headers = NULL;

    headers = curl_slist_append(headers,
                                "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    int ret = perform_request(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return ret;
}

int http_delete(const char *url)
{
    CURL *curl = curl_easy_init();

    if(!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    int ret = perform_request(curl);

    curl_easy_cleanup(curl);

    return ret;
}