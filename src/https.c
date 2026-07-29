/**
 * @file http.c
 * @brief HTTP/HTTPS communication module using libcurl.
 *
 * This module provides wrapper functions for performing HTTP and HTTPS
 * GET, POST, PUT, and DELETE requests using the libcurl library. It also
 * includes initialization and cleanup functions for the libcurl environment.
 */

#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

/**
 * @brief Initializes the libcurl library.
 *
 * This function must be called once before using any HTTP or HTTPS
 * communication functions.
 *
 * @return int
 * @retval 1 Initialization completed successfully.
 * @retval 0 Initialization failed.
 */
int http_init(void)
{
    return (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

/**
 * @brief Cleans up libcurl resources.
 *
 * Releases global resources allocated by the libcurl library.
 * This function should be called before the application exits.
 */
void http_cleanup(void)
{
    curl_global_cleanup();
}

/**
 * @brief Performs an HTTP request using a configured CURL handle.
 *
 * Sets connection and request timeouts, executes the request, and
 * validates the HTTP response status code.
 *
 * @param[in] curl Pointer to an initialized CURL handle.
 *
 * @return int
 * @retval 1 HTTP request completed successfully with a 2xx response.
 * @retval 0 Request failed or returned a non-success HTTP status code.
 */
static int perform_request(CURL *curl)
{
    /* Fail fast instead of hanging on a bad link */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK)
    {
        printf("CURL Error : %s\n", curl_easy_strerror(res));
        return 0;
    }

    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    printf("\nHTTP Status : %ld\n", code);

    return (code >= 200 && code < 300);
}

/**
 * @brief Sends an HTTP GET request.
 *
 * Performs an HTTP GET request to the specified URL.
 *
 * @param[in] url URL of the remote resource.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int http_get(const char *url)
{
    CURL *curl = curl_easy_init();

    if (!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);

    int ret = perform_request(curl);

    curl_easy_cleanup(curl);

    return ret;
}

/**
 * @brief Sends an HTTP POST request with JSON data.
 *
 * Performs an HTTP POST request using the supplied JSON payload.
 *
 * @param[in] url Destination URL.
 * @param[in] json JSON data to transmit.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int http_post(const char *url, const char *json)
{
    CURL *curl = curl_easy_init();

    if (!curl)
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

/**
 * @brief Sends an HTTP PUT request with JSON data.
 *
 * Performs an HTTP PUT request using the supplied JSON payload.
 *
 * @param[in] url Destination URL.
 * @param[in] json JSON data to transmit.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int http_put(const char *url, const char *json)
{
    CURL *curl = curl_easy_init();

    if (!curl)
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

/**
 * @brief Sends an HTTP DELETE request.
 *
 * Performs an HTTP DELETE request for the specified URL.
 *
 * @param[in] url Destination URL.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int http_delete(const char *url)
{
    CURL *curl = curl_easy_init();

    if (!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    int ret = perform_request(curl);

    curl_easy_cleanup(curl);

    return ret;
}

/**
 * @brief Sends an HTTPS GET request.
 *
 * This function is a thin wrapper around http_get(). The protocol
 * (HTTP or HTTPS) is determined by the URL scheme.
 *
 * @param[in] url HTTPS URL.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int https_get(const char *url)
{
    return http_get(url);
}

/**
 * @brief Sends an HTTPS POST request with JSON data.
 *
 * This function forwards the request to http_post().
 *
 * @param[in] url HTTPS URL.
 * @param[in] json JSON payload.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int https_post(const char *url, const char *json)
{
    return http_post(url, json);
}

/**
 * @brief Sends an HTTPS PUT request with JSON data.
 *
 * This function forwards the request to http_put().
 *
 * @param[in] url HTTPS URL.
 * @param[in] json JSON payload.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int https_put(const char *url, const char *json)
{
    return http_put(url, json);
}

/**
 * @brief Sends an HTTPS DELETE request.
 *
 * This function forwards the request to http_delete().
 *
 * @param[in] url HTTPS URL.
 *
 * @return int
 * @retval 1 Request completed successfully.
 * @retval 0 Request failed.
 */
int https_delete(const char *url)
{
    return http_delete(url);
}