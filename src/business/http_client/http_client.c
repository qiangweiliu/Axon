/*
 * http_client.c — HTTP/1.1 + HTTPS client
 *
 * HTTP:  raw TCP via os_socket.
 * HTTPS: pipes through curl (openssl s_client is unreliable on WSL).
 * Parses status line, Content-Length, reads body.
 *
 * Revision: 2026-06-20 — HTTPS switched from openssl s_client to curl
 * because openssl s_client with pipe subprocess does not produce output
 * on WSL (Win32 OpenSSL 1.1.1f).
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "http_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>

#define RECV_BUF        65536
#define HEADER_MAX      4096

/* Default timeout, can be overridden via http_set_timeout() */
static int g_http_timeout_sec = 120;

void http_set_timeout(int timeout_sec) {
    if (timeout_sec > 0) g_http_timeout_sec = timeout_sec;
}

static int atoi_simple(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

/* ── Response Parser ────────────────────────────────────────────────── */

static http_response_t *parse_response(char *buf, size_t total)
{
    if (!buf || total == 0) return NULL;

    http_response_t *resp = (http_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) return NULL;

    /* Parse status line: "HTTP/1.1 200 OK" */
    char *status_line = buf;
    char *sp = strchr(status_line, ' ');
    if (sp) {
        resp->status_code = atoi_simple(sp + 1);
    }

    /* Find header-body boundary: \r\n\r\n */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) {
        resp->body = buf;
        resp->body_len = total;
        return resp;
    }
    body_start += 4;

    /* Try Content-Length */
    char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (cl) {
        char *val = cl + 15;
        while (*val == ' ') val++;
        size_t content_len = (size_t)atoi_simple(val);
        size_t body_avail = total - (size_t)(body_start - buf);
        size_t copy_len = content_len < body_avail ? content_len : body_avail;
        if (copy_len > 0) {
            resp->body = (char *)os_alloc(copy_len + 1);
            if (resp->body) {
                os_memcpy(resp->body, body_start, copy_len);
                resp->body[copy_len] = '\0';
                resp->body_len = copy_len;
            }
        }
        return resp;
    }

    /* Chunked or unknown — take everything after headers */
    size_t body_avail = total - (size_t)(body_start - buf);
    if (body_avail > 0) {
        resp->body = (char *)os_alloc(body_avail + 1);
        if (resp->body) {
            os_memcpy(resp->body, body_start, body_avail);
            resp->body[body_avail] = '\0';
            resp->body_len = body_avail;
        }
    }
    return resp;
}

/* ── HTTPS via curl ─────────────────────────────────────────────────── */

static http_response_t *https_request(const char *method,
                                      const char *host, int port,
                                      const char *path,
                                      const char *content_type,
                                      const char *body,
                                      const char *extra_headers)
{
    if (!host || port <= 0 || !path) return NULL;
    if (!method) method = "GET";

    /* Build curl command line */
    char cmd[16384];
    int pos = 0;
    pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
        "curl -si --max-time %d -X %s 'https://%s:%d%s'",
        g_http_timeout_sec, method, host, port, path);

    if (content_type)
        pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
            " -H 'Content-Type: %s'", content_type);

    if (extra_headers) {
        const char *h = extra_headers;
        while (*h && pos < (int)sizeof(cmd) - 100) {
            while (*h == ' ' || *h == '\t') h++;
            if (!*h) break;
            const char *eol = strstr(h, "\r\n");
            if (!eol) eol = strchr(h, '\n');
            if (!eol) eol = h + os_strlen(h);
            size_t hlen = (size_t)(eol - h);
            if (hlen > 0 && hlen < 800) {
                char hbuf[1024];
                os_memcpy(hbuf, h, hlen);
                hbuf[hlen] = '\0';
                pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
                    " -H '%s'", hbuf);
            }
            if (*eol == '\r') eol++;
            if (*eol == '\n') eol++;
            h = eol;
        }
    }

    int body_len = body ? (int)os_strlen(body) : 0;
    if (body && body_len > 0) {
        /* Write body to temp file to avoid shell escaping issues */
        char tmp_path[] = "/tmp/http_body_XXXXXX";
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) return NULL;
        write(tmp_fd, body, (size_t)body_len);
        close(tmp_fd);
        pos += os_snprintf(cmd + pos, sizeof(cmd) - pos, " -d '@%s'", tmp_path);
    }

    /* Execute curl */
    LOG_DEBUG("HTTPS: %s", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char *buf = (char *)os_alloc(RECV_BUF);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0;
    while (total < RECV_BUF - 1) {
        size_t n = fread(buf + total, 1, RECV_BUF - total - 1, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);

    /* Clean up temp file AFTER curl has finished (not before — race on WSL) */
    if (body && body_len > 0) {
        const char *tp = strstr(cmd, "/tmp/http_body_");
        if (tp) unlink(tp);
    }

    /* Debug: dump raw response on HTTP error */
    const char *dbg = buf;
    while (*dbg == ' ' || *dbg == '\t' || *dbg == '\n' || *dbg == '\r') dbg++;
    if (total > 0 && (os_strncmp(dbg, "HTTP/", 5) != 0)) {
        LOG_DEBUG("HTTPS: raw output (first 200): %.*s",
                  (int)(total < 200 ? total : 200), buf);
    }

    return parse_response(buf, total);
}

/* ── HTTPS Streaming via curl (line by line) ────────────────────────── */

int https_post_stream(const char *host, int port,
                      const char *path,
                      const char *content_type,
                      const char *body,
                      const char *extra_headers,
                      http_chunk_cb_t on_chunk, void *user)
{
    if (!host || port <= 0 || !path || !on_chunk) return -1;

    /* Build curl command */
    char cmd[16384];
    int pos = 0;
    int body_len = body ? (int)os_strlen(body) : 0;

    pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
        "curl -sN --max-time %d -X POST 'https://%s:%d%s'",
        g_http_timeout_sec, host, port, path);

    if (content_type)
        pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
            " -H 'Content-Type: %s'", content_type);

    if (extra_headers) {
        const char *h = extra_headers;
        while (*h && pos < (int)sizeof(cmd) - 100) {
            while (*h == ' ' || *h == '\t') h++;
            if (!*h) break;
            const char *eol = strstr(h, "\r\n");
            if (!eol) eol = strchr(h, '\n');
            if (!eol) eol = h + os_strlen(h);
            size_t hlen = (size_t)(eol - h);
            if (hlen > 0 && hlen < 800) {
                char hbuf[1024];
                os_memcpy(hbuf, h, hlen);
                hbuf[hlen] = '\0';
                pos += os_snprintf(cmd + pos, sizeof(cmd) - pos,
                    " -H '%s'", hbuf);
            }
            if (*eol == '\r') eol++;
            if (*eol == '\n') eol++;
            h = eol;
        }
    }

    if (body && body_len > 0) {
        char tmp_path[] = "/tmp/http_body_XXXXXX";
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) return -1;
        write(tmp_fd, body, (size_t)body_len);
        close(tmp_fd);
        pos += os_snprintf(cmd + pos, sizeof(cmd) - pos, " -d '@%s'", tmp_path);
    }

    /* Execute curl with popen */
    LOG_DEBUG("HTTPS: STREAM %s", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    /* Read output line by line */
    char line[16384];
    size_t total = 0;
    while (fgets(line, sizeof(line), fp)) {
        size_t llen = os_strlen(line);
        on_chunk(line, llen, user);
        total += llen;
    }

    pclose(fp);

    /* Clean up temp file after curl finishes */
    if (body && body_len > 0) {
        const char *tp = strstr(cmd, "/tmp/http_body_");
        if (tp) unlink(tp);
    }

    return (int)total;
}

/* ── HTTP via raw TCP socket (non-TLS) ──────────────────────────────── */

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

    os_socket_set_timeout(fd, g_http_timeout_sec);

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

/* ── Public API ─────────────────────────────────────────────────────── */

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
