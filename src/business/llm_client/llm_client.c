/*
 * llm_client.c — OpenAI-compatible chat completion client
 *
 * Builds JSON request, calls http_post, extracts response content.
 * Minimal JSON parsing — no external library needed.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "http_client.h"
#include "llm_client.h"

#define BUF_SIZE 16384

/* ── URL Parse ────────────────────────────────────────────────────── */

static int parse_url(const char *url, char *host, size_t host_len,
                     int *port, char *path, size_t path_len, int *is_https)
{
    if (!url || !host || !port || !path) return -1;

    *is_https = 0;
    const char *p = url;
    if (os_strncmp(p, "https://", 8) == 0) {
        p += 8; *port = 443; *is_https = 1;
    } else if (os_strncmp(p, "http://", 7) == 0) {
        p += 7; *port = 80;
    } else { return -1; }

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t hlen = (size_t)(p - host_start);
    if (hlen >= host_len) return -1;
    os_memcpy(host, host_start, hlen); host[hlen] = '\0';

    if (*p == ':') {
        p++; *port = 0;
        while (*p >= '0' && *p <= '9') { *port = *port * 10 + (*p - '0'); p++; }
    }
    if (*p == '/') {
        size_t plen = os_strlen(p);
        if (plen >= path_len) return -1;
        os_memcpy(path, p, plen); path[plen] = '\0';
    } else {
        os_memcpy(path, "/", 2);
    }
    return 0;
}

/* ── JSON Builders ────────────────────────────────────────────────── */

static int build_chat_body(char *buf, size_t buf_len,
                           const char *model, const char *prompt)
{
    char safe_prompt[BUF_SIZE];
    size_t plen = os_strlen(prompt);
    if (plen >= sizeof(safe_prompt)) plen = sizeof(safe_prompt) - 1;
    for (size_t i = 0; i < plen; i++)
        safe_prompt[i] = (prompt[i] == '"') ? '\'' : prompt[i];
    safe_prompt[plen] = '\0';

    return os_snprintf(buf, buf_len,
        "{"
        "\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]"
        "}",
        model, safe_prompt);
}

/* ── JSON Extractor ───────────────────────────────────────────────── */

static char *extract_content(const char *json, size_t *out_len)
{
    const char *key = "\"content\":\"";
    const char *start = NULL, *p = json;
    while (*p) {
        const char *k = key, *s = p;
        while (*k && *s == *k) { s++; k++; }
        if (!*k) { start = s; break; }
        p++;
    }
    if (!start) return NULL;
    const char *end = start;
    while (*end && *end != '"') end++;
    size_t len = (size_t)(end - start);
    char *content = (char *)os_alloc(len + 1);
    if (!content) return NULL;
    os_memcpy(content, start, len);
    content[len] = '\0';
    if (out_len) *out_len = len;
    return content;
}

static int extract_int_field(const char *json, const char *field)
{
    const char *p = json;
    while (*p) {
        const char *k = field, *s = p;
        while (*k && *s == *k) { s++; k++; }
        if (!*k) {
            s--;
            if (*s == '"') {
                while (*s && *s != ':') s++;
                if (*s == ':') s++;
                while (*s == ' ' || *s == '\t') s++;
                int n = 0;
                while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
                return n;
            }
        }
        p++;
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

llm_response_t *llm_chat(const char *endpoint,
                         const char *api_key,
                         const char *model,
                         const char *prompt)
{
    if (!endpoint || !model || !prompt) return NULL;

    char host[256], path[512];
    int port = 80, is_https = 0;

    if (parse_url(endpoint, host, sizeof(host), &port,
                  path, sizeof(path), &is_https) != 0) {
        LOG_ERROR("LLM: bad endpoint '%s'", endpoint);
        return NULL;
    }

    char body[BUF_SIZE];
    int blen = build_chat_body(body, sizeof(body), model, prompt);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        LOG_ERROR("LLM: body too large");
        return NULL;
    }

    LOG_INFO("LLM: POST %s:%d%s (%s) [%s]",
             host, port, path, model, is_https ? "TLS" : "plain");

    /* Append /chat/completions */
    {
        size_t plen = os_strlen(path);
        int need_append = 1;
        if (plen >= 18) {
            const char *tail = path + plen - 18;
            if (os_strcmp(tail, "/chat/completions") == 0) need_append = 0;
        }
        if (need_append && plen + 18 < sizeof(path)) {
            if (path[plen-1] == '/')
                os_snprintf(path + plen, sizeof(path) - plen, "chat/completions");
            else
                os_snprintf(path + plen, sizeof(path) - plen, "/chat/completions");
        }
    }

    char *extra = NULL;
    char auth_hdr[256];
    if (api_key && api_key[0]) {
        os_snprintf(auth_hdr, sizeof(auth_hdr),
                    "Authorization: Bearer %s\r\n", api_key);
        extra = auth_hdr;
    }

    http_response_t *http;
    if (is_https) {
        http = https_post(host, port, path, "application/json", body, extra);
    } else {
        http = http_post(host, port, path, "application/json", body, extra);
    }
    if (!http) { LOG_ERROR("LLM: HTTP request failed"); return NULL; }

    LOG_INFO("LLM: HTTP %d, body_len=%zu", http->status_code, http->body_len);
    if (http->status_code != 200 || !http->body) {
        http_response_free(http); return NULL;
    }

    llm_response_t *resp = (llm_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) { http_response_free(http); return NULL; }

    resp->content = extract_content(http->body, &resp->content_len);
    resp->prompt_tokens = extract_int_field(http->body, "prompt_tokens");
    resp->completion_tokens = extract_int_field(http->body, "completion_tokens");
    http_response_free(http);

    if (!resp->content) { LOG_WARN("LLM: no content"); os_free(resp); return NULL; }
    LOG_INFO("LLM: response (%zu chars)", resp->content_len);
    return resp;
}

void llm_response_free(llm_response_t *resp)
{
    if (!resp) return;
    if (resp->content) os_free(resp->content);
    os_free(resp);
}

/* ── Streaming ────────────────────────────────────────────────────── */

typedef struct {
    char  buf[65536];
    size_t pos;
    char *accumulated;
    size_t accum_len;
    size_t accum_cap;
    int    tokens;
    int    done;
    int    prompt_tokens;
    int    completion_tokens;
    llm_token_cb_t cb;
    void  *cb_user;
    uint64_t t0;
} sse_parser_t;

static void sse_feed(sse_parser_t *sp, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (sp->pos >= sizeof(sp->buf) - 1) sp->pos = 0;
        sp->buf[sp->pos++] = data[i];
        sp->buf[sp->pos] = '\0';

        if (sp->pos >= 2 && sp->buf[sp->pos-1] == '\n' &&
            sp->buf[sp->pos-2] == '\n') {
            char *line = sp->buf;
            while (line && *line && line < sp->buf + sp->pos) {
                if (os_strncmp(line, "data: ", 6) == 0) {
                    char *json = line + 6;
                    char *end = json + os_strlen(json);
                    while (end > json && (*end == '\n' || *end == '\r'))
                        { *end = '\0'; end--; }
                    if (os_strcmp(json, "[DONE]") == 0) { sp->done = 1; }
                    else {
                        char *content = extract_content(json, NULL);
                        if (content && content[0]) {
                            sp->tokens++;
                            uint64_t elapsed = os_clock_ms() - sp->t0;
                            if (sp->cb)
                                sp->cb(content, os_strlen(content),
                                       sp->tokens, elapsed, sp->cb_user);
                            size_t clen = os_strlen(content);
                            if (sp->accum_len + clen + 1 > sp->accum_cap) {
                                sp->accum_cap = sp->accum_len + clen + 8192;
                                char *nb = (char *)os_realloc(
                                    sp->accumulated, sp->accum_cap);
                                if (!nb) break;
                                sp->accumulated = nb;
                            }
                            os_memcpy(sp->accumulated + sp->accum_len, content, clen);
                            sp->accum_len += clen;
                            sp->accumulated[sp->accum_len] = '\0';
                            int ct = extract_int_field(json, "completion_tokens");
                            if (ct > sp->completion_tokens) sp->completion_tokens = ct;
                            int pt = extract_int_field(json, "prompt_tokens");
                            if (pt > sp->prompt_tokens) sp->prompt_tokens = pt;
                        }
                        if (content) os_free(content);
                    }
                }
                while (*line && *line != '\n') line++;
                if (*line == '\n') line++;
            }
            sp->pos = 0;
        }
    }
}

static void sse_callback(const char *data, size_t len, void *user)
{
    sse_feed((sse_parser_t *)user, data, len);
}

llm_response_t *llm_chat_stream(const char *endpoint,
                                const char *api_key,
                                const char *model,
                                const char *prompt,
                                llm_token_cb_t on_token, void *user)
{
    if (!endpoint || !model || !prompt) return NULL;
    char host[256], path[512]; int port = 80, is_https = 0;
    if (parse_url(endpoint, host, sizeof(host), &port,
                  path, sizeof(path), &is_https) != 0) {
        LOG_ERROR("LLM: bad endpoint '%s'", endpoint); return NULL;
    }

    char safe_prompt[BUF_SIZE];
    size_t plen = os_strlen(prompt);
    if (plen >= sizeof(safe_prompt)) plen = sizeof(safe_prompt) - 1;
    for (size_t i = 0; i < plen; i++)
        safe_prompt[i] = (prompt[i] == '"') ? '\'' : prompt[i];
    safe_prompt[plen] = '\0';

    char body[BUF_SIZE];
    int blen = os_snprintf(body, sizeof(body),
        "{"
        "\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"stream\":true"
        "}", model, safe_prompt);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        LOG_ERROR("LLM: body too large"); return NULL;
    }

    LOG_INFO("LLM: STREAM %s:%d%s (%s) [%s]",
             host, port, path, model, is_https ? "TLS" : "plain");

    /* Append /chat/completions */
    {
        size_t pl = os_strlen(path);
        if (pl < 18 || os_strcmp(path + pl - 18, "/chat/completions") != 0) {
            if (pl + 18 < sizeof(path)) {
                if (path[pl-1] == '/')
                    os_snprintf(path + pl, sizeof(path) - pl, "chat/completions");
                else
                    os_snprintf(path + pl, sizeof(path) - pl, "/chat/completions");
            }
        }
    }

    char *extra = NULL;
    char auth_hdr[256];
    if (api_key && api_key[0]) {
        os_snprintf(auth_hdr, sizeof(auth_hdr),
                    "Authorization: Bearer %s\r\n", api_key);
        extra = auth_hdr;
    }

    sse_parser_t sp;
    os_memset(&sp, 0, sizeof(sp));
    sp.cb = on_token; sp.cb_user = user;
    sp.t0 = os_clock_ms();
    sp.accum_cap = 8192;
    sp.accumulated = (char *)os_alloc(sp.accum_cap);
    if (!sp.accumulated) return NULL;
    sp.accumulated[0] = '\0';

    int total;
    if (is_https) {
        total = https_post_stream(host, port, path,
                                  "application/json", body, extra,
                                  sse_callback, &sp);
    } else {
        LOG_ERROR("LLM: streaming requires HTTPS");
        os_free(sp.accumulated); return NULL;
    }

    if (total < 0) { LOG_ERROR("LLM: stream failed"); os_free(sp.accumulated); return NULL; }

    sse_feed(&sp, "\n\n", 2);

    if (sp.accum_len == 0) { LOG_WARN("LLM: no content"); os_free(sp.accumulated); return NULL; }

    llm_response_t *resp = (llm_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) { os_free(sp.accumulated); return NULL; }
    resp->content = sp.accumulated;
    resp->content_len = sp.accum_len;
    resp->prompt_tokens = sp.prompt_tokens;
    resp->completion_tokens = sp.completion_tokens;
    if (resp->completion_tokens == 0 && sp.tokens > 0)
        resp->completion_tokens = sp.tokens;

    LOG_INFO("LLM: stream done (%zu chars, %d tok)",
             resp->content_len, resp->completion_tokens);
    return resp;
}

/* ── Module Registration ──────────────────────────────────────────── */

static int llm_client_init(framework_module_t *mod)
{
    (void)mod; LOG_INFO("LLM: init"); return 0;
}
static int llm_client_start(framework_module_t *mod)
{
    (void)mod; LOG_INFO("LLM: ready"); return 0;
}
framework_module_t llm_client_mod = {
    .name = "llm_client", .version = 0x00010000, .priority = 350,
    .state = FRAMEWORK_STATE_UNLOADED, .init = llm_client_init,
    .start = llm_client_start, .loop = NULL, .stop = NULL,
    .deinit = NULL, .ctx = NULL, .id = 0, .next = NULL,
};
MODULE_REGISTER(llm_client_mod);
