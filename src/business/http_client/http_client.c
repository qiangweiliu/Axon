/*
 * http_client.c — HTTP/1.1 + HTTPS client
 *
 * HTTP:  raw TCP via os_socket.
 * HTTPS: pipes through openssl s_client (zero library deps).
 * Parses status line, Content-Length, reads body.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "http_client.h"
#include <unistd.h>
#include <fcntl.h>

#define RECV_BUF    32768
#define HEADER_MAX  4096

static int atoi_simple(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

/* Simple chunked transfer decoding: buf is modified in-place.
   Returns length of decoded body (without chunk headers/trailers). */
static size_t dechunk(char *buf)
{
    char *src = buf, *dst = buf;
    while (*src) {
        long size = 0;
        while (*src >= '0' && *src <= '9') { size = size * 16 + (*src - '0'); src++; }
        while (*src >= 'a' && *src <= 'f') { size = size * 16 + (*src - 'a' + 10); src++; }
        while (*src >= 'A' && *src <= 'F') { size = size * 16 + (*src - 'A' + 10); src++; }
        while (*src == '\r' || *src == '\n') src++;
        if (size <= 0) break;
        char *chunk_end = src + size;
        while (src < chunk_end && *src) *dst++ = *src++;
        while (*src == '\r' || *src == '\n') src++;
    }
    *dst = '\0';
    return (size_t)(dst - buf);
}

static int str_case_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int parse_status(const char *response)
{
    const char *p = response;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;
    return atoi_simple(p);
}

static int parse_content_length(const char *headers)
{
    const char *p = headers;
    while (*p) {
        if (str_case_prefix(p, "content-length:")) {
            p += 15; while (*p == ' ' || *p == '\t') p++;
            return atoi_simple(p);
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return -1;
}

static const char *find_body_start(const char *response)
{
    const char *p = response;
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        if (p[0] == '\n' && p[1] == '\n')
            return p + 2;
        p++;
    }
    return NULL;
}

static http_response_t *parse_response(char *buf, size_t total)
{
    if (total == 0) return NULL;

    int status = parse_status(buf);
    int clen = parse_content_length(buf);
    const char *body_start = find_body_start(buf);

    http_response_t *resp = (http_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) return NULL;
    resp->status_code = status;

    if (body_start) {
        size_t before_body = (size_t)(body_start - buf);
        size_t available = total - before_body;
        size_t copy_len;

        if (clen > 0) {
            /* Content-Length based */
            copy_len = (size_t)clen < available ? (size_t)clen : available;
        } else {
            /* Chunked or unknown — take everything after headers */
            copy_len = available;
            if (copy_len > 0) {
                resp->body = (char *)os_alloc(copy_len + 1);
                if (resp->body) {
                    os_memcpy(resp->body, body_start, copy_len);
                    resp->body[copy_len] = '\0';
                    resp->body_len = dechunk(resp->body);
                }
            }
        }
    }
    return resp;
}

/* ── HTTPS via openssl s_client pipe ───────────────────────────────── */

static http_response_t *https_request(const char *method,
                                      const char *host, int port,
                                      const char *path,
                                      const char *content_type,
                                      const char *body,
                                      const char *extra_headers)
{
    if (!host || port <= 0 || !path) return NULL;
    if (!method) method = "GET";

    /* Build HTTP request */
    char req[RECV_BUF];
    int body_len = body ? (int)os_strlen(body) : 0;
    int hdr_len;
    const char *ext = extra_headers ? extra_headers : "";

    if (body && content_type) {
        hdr_len = os_snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\n"
            "Content-Type: %s\r\nContent-Length: %d\r\n%s"
            "Connection: close\r\n\r\n",
            method, path, host, content_type, body_len, ext);
    } else {
        hdr_len = os_snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\n%s"
            "Connection: close\r\n\r\n",
            method, path, host, ext);
    }
    if (hdr_len < 0 || hdr_len >= (int)sizeof(req)) return NULL;
    if (body && body_len > 0) {
        int r = os_snprintf(req + hdr_len, sizeof(req) - (size_t)hdr_len,
                            "%s", body);
        if (r < 0) return NULL;
        hdr_len += r;
    }

    /* Create two pipes: parent reads from p_out, writes to p_in */
    int p_in[2], p_out[2];
    if (os_pipe(p_in) != 0 || os_pipe(p_out) != 0) return NULL;

    char port_str[16];
    os_snprintf(port_str, sizeof(port_str), "%d", port);

    os_pid_t pid = os_proc_fork();
    if (pid < 0) {
        os_fd_close(p_in[0]); os_fd_close(p_in[1]);
        os_fd_close(p_out[0]); os_fd_close(p_out[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child: exec openssl s_client -quiet -connect host:port */
        os_dup2(p_in[0], 0);   /* stdin  from parent */
        os_dup2(p_out[1], 1);  /* stdout to parent */
        os_fd_close(p_in[0]); os_fd_close(p_in[1]);
        os_fd_close(p_out[0]); os_fd_close(p_out[1]);

        /* Redirect stderr to /dev/null to suppress cert output */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { os_dup2(devnull, 2); os_fd_close(devnull); }

        char *argv[] = { "openssl", "s_client", "-quiet",
                         "-connect", NULL, NULL };
        char conn[256];
        os_snprintf(conn, sizeof(conn), "%s:%s", host, port_str);
        argv[4] = conn;
        os_proc_exec("openssl", argv);
        _exit(1);
    }

    /* Parent: close unused pipe ends */
    os_fd_close(p_in[0]);
    os_fd_close(p_out[1]);

    /* Write request to child's stdin (pipe fd, use write not send) */
    write(p_in[1], req, (size_t)hdr_len);
    os_fd_close(p_in[1]);  /* close stdin → openssl knows we're done */

    /* Read response from child's stdout (pipe fd, use read not recv) */
    char *buf = (char *)os_alloc(RECV_BUF);
    if (!buf) { os_fd_close(p_out[0]); os_proc_wait(pid, NULL); return NULL; }

    size_t total = 0;
    for (;;) {
        ssize_t nr = read(p_out[0], buf + total, RECV_BUF - total - 1);
        if (nr <= 0) break;
        total += (size_t)nr;
        if (total >= RECV_BUF - 1) break;
    }
    buf[total] = '\0';
    os_fd_close(p_out[0]);
    os_proc_wait(pid, NULL);

    http_response_t *resp = parse_response(buf, total);
    os_free(buf);
    return resp;
}

/* ── HTTPS Streaming ──────────────────────────────────────────────── */

int https_post_stream(const char *host, int port,
                      const char *path,
                      const char *content_type,
                      const char *body,
                      const char *extra_headers,
                      http_chunk_cb_t on_chunk, void *user)
{
    if (!host || port <= 0 || !path || !on_chunk) return -1;

    char req[RECV_BUF];
    int body_len = body ? (int)os_strlen(body) : 0;
    int hdr_len;
    const char *ext = extra_headers ? extra_headers : "";

    if (body && content_type) {
        hdr_len = os_snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: %s\r\n"
            "Content-Type: %s\r\nContent-Length: %d\r\n%s"
            "Connection: close\r\n\r\n",
            path, host, content_type, body_len, ext);
    } else {
        hdr_len = os_snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: %s\r\n%s"
            "Connection: close\r\n\r\n",
            path, host, ext);
    }
    if (hdr_len < 0 || hdr_len >= (int)sizeof(req)) return -1;
    if (body && body_len > 0) {
        int r = os_snprintf(req + hdr_len, sizeof(req) - (size_t)hdr_len,
                            "%s", body);
        if (r < 0) return -1;
        hdr_len += r;
    }

    int p_in[2], p_out[2];
    if (os_pipe(p_in) != 0 || os_pipe(p_out) != 0) return -1;

    char port_str[16];
    os_snprintf(port_str, sizeof(port_str), "%d", port);

    os_pid_t pid = os_proc_fork();
    if (pid < 0) {
        os_fd_close(p_in[0]); os_fd_close(p_in[1]);
        os_fd_close(p_out[0]); os_fd_close(p_out[1]);
        return -1;
    }

    if (pid == 0) {
        os_dup2(p_in[0], 0);
        os_dup2(p_out[1], 1);
        os_fd_close(p_in[0]); os_fd_close(p_in[1]);
        os_fd_close(p_out[0]); os_fd_close(p_out[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { os_dup2(devnull, 2); os_fd_close(devnull); }
        char conn[256];
        os_snprintf(conn, sizeof(conn), "%s:%s", host, port_str);
        char *argv[] = { "openssl", "s_client", "-quiet",
                         "-connect", conn, NULL };
        os_proc_exec("openssl", argv);
        _exit(1);
    }

    os_fd_close(p_in[0]);
    os_fd_close(p_out[1]);

    write(p_in[1], req, (size_t)hdr_len);
    os_fd_close(p_in[1]);

    /* Read chunks and call callback for each */
    char buf[4096];
    size_t total = 0;
    for (;;) {
        ssize_t nr = read(p_out[0], buf, sizeof(buf));
        if (nr <= 0) break;
        on_chunk(buf, (size_t)nr, user);
        total += (size_t)nr;
    }

    os_fd_close(p_out[0]);
    os_proc_wait(pid, NULL);
    return (int)total;
}

/* ── HTTP via raw TCP socket ───────────────────────────────────────── */

static http_response_t *http_request(const char *method,
                                     const char *host, int port,
                                     const char *path,
                                     const char *content_type,
                                     const char *body,
                                     const char *extra_headers)
{
    if (!host || port <= 0 || !path) return NULL;
    if (!method) method = "GET";

    os_socket_t fd = os_socket_create(2, 1, 0);
    if (fd < 0) return NULL;

    char ip[64];
    if (os_resolve_host(host, ip, sizeof(ip)) != 0) {
        os_socket_close(fd); return NULL;
    }

    if (os_socket_connect(fd, ip, port) != 0) {
        os_socket_close(fd); return NULL;
    }

    char req[RECV_BUF];
    int body_len = body ? (int)os_strlen(body) : 0;
    int hdr_len;
    const char *ext = extra_headers ? extra_headers : "";

    if (body && content_type) {
        hdr_len = os_snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
            "Content-Type: %s\r\nContent-Length: %d\r\n%s"
            "Connection: close\r\n\r\n",
            method, path, host, port, content_type, body_len, ext);
    } else {
        hdr_len = os_snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%s"
            "Connection: close\r\n\r\n",
            method, path, host, port, ext);
    }

    if (hdr_len < 0 || hdr_len >= (int)sizeof(req)) {
        os_socket_close(fd); return NULL;
    }

    os_socket_send(fd, req, (size_t)hdr_len, 0);
    if (body && body_len > 0)
        os_socket_send(fd, body, (size_t)body_len, 0);

    char *buf = (char *)os_alloc(RECV_BUF);
    if (!buf) { os_socket_close(fd); return NULL; }

    size_t total = 0;
    for (;;) {
        ssize_t nr = os_socket_recv(fd, buf + total, RECV_BUF - total - 1, 0);
        if (nr <= 0) break;
        total += (size_t)nr;
        if (total >= RECV_BUF - 1) break;
    }
    buf[total] = '\0';
    os_socket_close(fd);

    http_response_t *resp = parse_response(buf, total);
    os_free(buf);
    return resp;
}

/* ── Public API ───────────────────────────────────────────────────── */

http_response_t *http_get(const char *host, int port, const char *path)
{
    return http_request("GET", host, port, path, NULL, NULL, NULL);
}

http_response_t *http_post(const char *host, int port,
                           const char *path,
                           const char *content_type,
                           const char *body,
                           const char *extra_headers)
{
    return http_request("POST", host, port, path,
                        content_type, body, extra_headers);
}

http_response_t *https_post(const char *host, int port,
                            const char *path,
                            const char *content_type,
                            const char *body,
                            const char *extra_headers)
{
    return https_request("POST", host, port, path,
                         content_type, body, extra_headers);
}

void http_response_free(http_response_t *resp)
{
    if (!resp) return;
    if (resp->body) os_free(resp->body);
    os_free(resp);
}

/* ── Module Registration ──────────────────────────────────────────── */

static int http_client_init(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("HttpClient: init (HTTP+HTTPS)");
    return 0;
}

static int http_client_start(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("HttpClient: ready");
    return 0;
}

    framework_module_t http_client_mod = {
    .name     = "http_client",
    .version  = 0x00020000,
    
    .state    = FRAMEWORK_STATE_UNLOADED,
    .init     = http_client_init,
    .start    = http_client_start,
    .loop     = NULL, .stop = NULL, .deinit = NULL,
    .ctx      = NULL, .id = 0, .next = NULL,
};

MODULE_REGISTER(http_client_mod);
