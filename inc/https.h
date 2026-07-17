#ifndef HTTPS_H
#define HTTPS_H

/* Call once at startup / shutdown */
int  http_init(void);
void http_cleanup(void);

/* Core functions — work for both http:// and https:// URLs */
int http_get(const char *url);
int http_post(const char *url, const char *json);
int http_put(const char *url, const char *json);
int http_delete(const char *url);

/* Aliases — identical behavior, just named for HTTPS call sites */
int https_get(const char *url);
int https_post(const char *url, const char *json);
int https_put(const char *url, const char *json);
int https_delete(const char *url);

#endif /* HTTPS_H */