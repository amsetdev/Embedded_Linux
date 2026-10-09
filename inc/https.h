/**
 * @file https.h
 * @brief HTTP and HTTPS client interface.
 *
 * This module provides wrapper functions for sending HTTP and HTTPS
 * requests using libcurl. It supports GET, POST, PUT, and DELETE
 * methods and includes initialization and cleanup functions for the
 * underlying networking library.
 */

#ifndef HTTPS_H
#define HTTPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Library Initialization                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the HTTP/HTTPS client library.
 *
 * This function must be called once before using any HTTP or HTTPS
 * request functions.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int http_init(void);

/**
 * @brief Releases resources used by the HTTP/HTTPS client library.
 *
 * Call this function before the application exits.
 */
void http_cleanup(void);

/* -------------------------------------------------------------------------- */
/* HTTP Request Functions                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Sends an HTTP GET request.
 *
 * @param url Target URL.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int http_get(const char *url);

/**
 * @brief Sends an HTTP POST request with a JSON payload.
 *
 * @param url Target URL.
 * @param json JSON payload to send.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int http_post(const char *url, const char *json);

/**
 * @brief Sends an HTTP PUT request with a JSON payload.
 *
 * @param url Target URL.
 * @param json JSON payload to send.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int http_put(const char *url, const char *json);

/**
 * @brief Sends an HTTP DELETE request.
 *
 * @param url Target URL.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int http_delete(const char *url);

/* -------------------------------------------------------------------------- */
/* HTTPS Request Functions                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Sends an HTTPS GET request.
 *
 * This function is an alias for ::http_get().
 *
 * @param url Target HTTPS URL.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int https_get(const char *url);

/**
 * @brief Sends an HTTPS POST request with a JSON payload.
 *
 * This function is an alias for ::http_post().
 *
 * @param url Target HTTPS URL.
 * @param json JSON payload to send.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int https_post(const char *url, const char *json);

/**
 * @brief Sends an HTTPS PUT request with a JSON payload.
 *
 * This function is an alias for ::http_put().
 *
 * @param url Target HTTPS URL.
 * @param json JSON payload to send.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int https_put(const char *url, const char *json);

/**
 * @brief Sends an HTTPS DELETE request.
 *
 * This function is an alias for ::http_delete().
 *
 * @param url Target HTTPS URL.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int https_delete(const char *url);

/* -------------------------------------------------------------------------- */
/* File Download                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Downloads a file from a URL to local storage.
 *
 * Uses libcurl to download the file at the given URL and write it
 * to the specified local path. Partial files are removed on failure.
 *
 * @param url       Source URL to download from.
 * @param filepath  Local path to write the downloaded file.
 * @param timeout_sec  Total transfer timeout in seconds (0 = default 300s).
 *
 * @return
 * - 1 on success.
 * - 0 on failure.
 */
int https_download_file(const char *url,
                        const char *filepath,
                        long timeout_sec);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_H */