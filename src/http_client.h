#ifndef BLIKK_HTTP_CLIENT_H
#define BLIKK_HTTP_CLIENT_H

#include <stddef.h>

/* Growable response body buffer. */
typedef struct {
    char *data;   /* NUL-terminated; NULL if nothing was received */
    size_t len;   /* length excluding the NUL terminator */
} http_buffer_t;

typedef struct {
    long status_code;          /* 0 if the request never completed */
    http_buffer_t body;
    long retry_after_seconds;  /* parsed "Retry-After" header, or -1 if absent */
} http_response_t;

/* Call once at program start / end, wraps curl_global_init/cleanup. */
int http_global_init(void);
void http_global_cleanup(void);

/*
 * Perform an HTTP GET with an "Authorization: Bearer <bearer_token>" header
 * (pass NULL to omit it). On success (request executed, regardless of HTTP
 * status) returns 0 and fills *resp. On a transport-level failure returns -1
 * and writes a message into err (if non-NULL/err_size>0).
 */
int http_get(const char *url, const char *bearer_token,
             http_response_t *resp, char *err, size_t err_size);

/*
 * Perform an HTTP POST with an empty body and an
 * "Authorization: Basic <basic_b64>" header.
 */
int http_post_basic_auth(const char *url, const char *basic_b64,
                          http_response_t *resp, char *err, size_t err_size);

void http_response_free(http_response_t *resp);

#endif /* BLIKK_HTTP_CLIENT_H */
