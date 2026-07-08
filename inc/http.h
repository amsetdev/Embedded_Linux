#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/** Call once at startup (mirrors mqtt_init()). Sets up libcurl globals. */
int  http_init(void);

/** POST a JSON payload to url. Returns 1 on HTTP 2xx, 0 otherwise.
 *  On success, if resp_buf is non-NULL, the response body is copied in
 *  (truncated to resp_buf_len - 1). Pass resp_buf=NULL to ignore the body. */
int  http_post_json(const char *url, const char *json_payload,
                     char *resp_buf, size_t resp_buf_len);

/** Simple GET, prints status + body to stdout. Returns 1 on HTTP 2xx. */
int  http_get(const char *url);

/** Call once at shutdown (mirrors mqtt_cleanup()). */
void http_cleanup(void);

#endif /* HTTP_H */