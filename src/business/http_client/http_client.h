/*
 * http_client.h — HTTP/1.1 + HTTPS client API
 *
 * Business layer (priority=300). HTTP uses raw TCP via os_socket.
 * HTTPS pipes through openssl s_client (zero library deps).
 */

#ifndef BUSINESS_HTTP_CLIENT_H
#define BUSINESS_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int   status_code;
    char *body;
    size_t body_len;
} http_response_t;

void http_response_free(http_response_t *resp);

/* HTTP (plain TCP) */
http_response_t *http_get(const char *host, int port, const char *path);

http_response_t *http_post(const char *host, int port,
                           const char *path,
                           const char *content_type,
                           const char *body,
                           const char *extra_headers);

/* HTTPS (via openssl s_client pipe) */
http_response_t *https_post(const char *host, int port,
                            const char *path,
                            const char *content_type,
                            const char *body,
                            const char *extra_headers);

/*
 * HTTPS streaming POST — each chunk of response is passed
 * to on_chunk(data, len, user). The caller manages response
 * parsing. Returns number of bytes received, or -1 on error.
 */
typedef void (*http_chunk_cb_t)(const char *data, size_t len, void *user);
int https_post_stream(const char *host, int port,
                      const char *path,
                      const char *content_type,
                      const char *body,
                      const char *extra_headers,
                      http_chunk_cb_t on_chunk, void *user);

/* Override the default socket timeout (seconds). 0 = keep default (120). */
void http_set_timeout(int timeout_sec);

#endif /* BUSINESS_HTTP_CLIENT_H */
