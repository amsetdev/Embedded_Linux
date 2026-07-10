#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

int http_init(void);
void http_cleanup(void);

int http_get(const char *url);

int http_post(const char *url, const char *json);

int http_put(const char *url, const char *json);

int http_delete(const char *url);

#endif