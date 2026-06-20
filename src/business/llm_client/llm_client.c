/*
 * llm_client.c — LLM API client core layer
 *
 * Dispatches model-specific request building and response parsing
 * to the selected model adapter (llm_core.h).
 * Handles HTTPS communication, streaming SSE, and unified response type.
 *
 * Layers:
 *   llm_client.c   ← core: HTTP, dispatch, public API
 *   llm_core.h     ← adapter interface
 *   model_*.c      ← per-model format handling
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "llm_client.h"
#include "llm_core.h"
#include "http_client.h"
#include <string.h>

#define BUF_SIZE    65536

/* ── Model discovery via .llm_models section scan ──────────────────── */

/* Symbols injected by linker script agent.ld */
extern const llm_model_t *__start___llm_models[];
extern const llm_model_t *__stop___llm_models[];

typedef struct {
    const llm_model_t *default_model;
} llm_client_ctx_t;

static llm_client_ctx_t *g_ctx = NULL;

void llm_model_discover(void)
{
    size_t count = (size_t)(__stop___llm_models - __start___llm_models);
    for (size_t i = 0; i < count; i++) {
        const llm_model_t *m = __start___llm_models[i];
        if (!m) continue;
        if (!g_ctx->default_model) g_ctx->default_model = m;
        LOG_INFO("LLM: model '%s' registered (%s)", m->name, m->build_body ? "ready" : "broken");
    }
    if (!g_ctx->default_model)
        LOG_WARN("LLM: no model adapters registered");
}

const llm_model_t* llm_model_select(const char *model_name)
{
    if (!model_name || !g_ctx->default_model) return g_ctx->default_model;
    size_t count = (size_t)(__stop___llm_models - __start___llm_models);
    for (size_t i = 0; i < count; i++) {
        const llm_model_t *m = __start___llm_models[i];
        if (!m) continue;
        if (os_strncmp(model_name, m->name,
                       os_strlen(m->name)) == 0)
            return m;
    }
    return g_ctx->default_model;
}

/* ── URL parsing ────────────────────────────────────────────────── */

static int parse_url(const char *url, char *host, size_t host_len,
                     int *port, char *path, size_t path_len, int *is_https)
{
    if (!url || !host || !port || !path || !is_https) return -1;
    const char *p = url;

    if (os_strncmp(p, "https://", 8) == 0)  { *is_https = 1; p += 8; }
    else if (os_strncmp(p, "http://", 7) == 0) { *is_https = 0; p += 7; }
    else return -1;

    const char *colon = os_strchr(p, ':');
    const char *slash = os_strchr(p, '/');
    const char *host_end;

    if (colon && (!slash || colon < slash)) {
        host_end = colon;
        *port = 0;
        for (const char *c = colon + 1; *c >= '0' && *c <= '9'; c++)
            *port = *port * 10 + (*c - '0');
    } else if (slash) {
        host_end = slash;
        *port = *is_https ? 443 : 80;
    } else {
        host_end = p + os_strlen(p);
        *port = *is_https ? 443 : 80;
    }

    size_t hlen = (size_t)(host_end - p);
    if (hlen >= host_len) hlen = host_len - 1;
    os_memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (slash) {
        size_t slen = os_strlen(slash);
        if (slen >= path_len) slen = path_len - 1;
        os_memcpy(path, slash, slen);
        path[slen] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    if (*port == 0) *port = *is_https ? 443 : 80;
    return 0;
}

/* ── SSE Parser (streaming) ─────────────────────────────────────── */

typedef struct {
    char   buf[65536];
    size_t pos;
    char  *accumulated;
    size_t accum_len;
    size_t accum_cap;
    int    tokens;
    int    done;
    int    prompt_tokens;
    int    completion_tokens;
    const llm_model_t *model;   /* adapter used for parsing */
    llm_token_cb_t cb;
    void  *cb_user;
    uint64_t t0;
} sse_parser_t;

static void sse_feed(sse_parser_t *sp, const char *data, size_t len)
{
    const llm_model_t *m = sp->model;
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
                        int extracted_is_reasoning = 0;
                        char *content = m->extract_content(json, NULL, &extracted_is_reasoning);
                        if (content && content[0]) {
                            sp->tokens++;
                            uint64_t elapsed = os_clock_ms() - sp->t0;
                            if (sp->cb)
                                sp->cb(content, os_strlen(content),
                                       sp->tokens, elapsed,
                                       extracted_is_reasoning, sp->cb_user);
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
                            int ct = m->extract_int(json, "completion_tokens");
                            if (ct > sp->completion_tokens) sp->completion_tokens = ct;
                            int pt = m->extract_int(json, "prompt_tokens");
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

/* ── Non-streaming request (llm_chat) ───────────────────────────── */

llm_response_t *llm_chat(const char *endpoint,
                          const char *api_key,
                          const char *model,
                          const char *prompt)
{
    if (!endpoint || !model || !prompt) return NULL;

    char host[256], path[512]; int port = 80, is_https = 0;
    if (parse_url(endpoint, host, sizeof(host), &port,
                  path, sizeof(path), &is_https) != 0) {
        LOG_ERROR("LLM: bad endpoint '%s'", endpoint); return NULL;
    }

    const llm_model_t *m = llm_model_select(model);
    char body[BUF_SIZE];
    int blen = m->build_body(body, sizeof(body), model, prompt, 0);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        LOG_ERROR("LLM: body too large"); return NULL;
    }

    LOG_DEBUG("LLM: POST %s:%d%s (%s) [%s] via %s",
             host, port, path, model, is_https ? "TLS" : "plain", m->name);

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
                    "Authorization: Bearer %s",
                    api_key);
        {
            size_t _alen = os_strlen(auth_hdr);
            if (_alen + 3 <= sizeof(auth_hdr)) {
                auth_hdr[_alen] = '\r';
                auth_hdr[_alen+1] = '\n';
                auth_hdr[_alen+2] = '\0';
            }
        }
        extra = auth_hdr;
    }

    http_response_t *http;
    if (is_https) {
        http = https_post(host, port, path, "application/json", body, extra);
    } else {
        http = http_post(host, port, path, "application/json", body, extra);
    }
    if (!http) { LOG_ERROR("LLM: HTTP request failed"); return NULL; }

    LOG_DEBUG("LLM: HTTP %d, body_len=%zu", http->status_code, http->body_len);
    if (http->status_code != 200 || !http->body) {
        http_response_free(http); return NULL;
    }

    llm_response_t *resp = (llm_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) { http_response_free(http); return NULL; }
    int dummy_is_reasoning = 0;
    resp->content = m->extract_content(http->body, &resp->content_len, &dummy_is_reasoning);
    resp->prompt_tokens = m->extract_int(http->body, "prompt_tokens");
    resp->completion_tokens = m->extract_int(http->body, "completion_tokens");
    if (!resp->content) { LOG_WARN("LLM: no content"); os_free(resp); http_response_free(http); return NULL; }
    LOG_DEBUG("LLM: response (%zu chars)", resp->content_len);
    http_response_free(http);
    return resp;
}

void llm_response_free(llm_response_t *resp)
{
    if (!resp) return;
    if (resp->content) os_free(resp->content);
    os_free(resp);
}

/* ── Streaming request (llm_chat_stream) ────────────────────────── */

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

    const llm_model_t *m = llm_model_select(model);
    char body[BUF_SIZE];
    int blen = m->build_body(body, sizeof(body), model, prompt, 1);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        LOG_ERROR("LLM: body too large"); return NULL;
    }

    LOG_DEBUG("LLM: STREAM %s:%d%s (%s) [%s] via %s",
             host, port, path, model, is_https ? "TLS" : "plain", m->name);

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
                    "Authorization: Bearer %s",
                    api_key);
        {
            size_t _alen = os_strlen(auth_hdr);
            if (_alen + 3 <= sizeof(auth_hdr)) {
                auth_hdr[_alen] = '\r';
                auth_hdr[_alen+1] = '\n';
                auth_hdr[_alen+2] = '\0';
            }
        }
        extra = auth_hdr;
    }

    sse_parser_t sp;
    os_memset(&sp, 0, sizeof(sp));
    sp.cb = on_token; sp.cb_user = user;
    sp.t0 = os_clock_ms();
    sp.accum_cap = 8192;
    sp.accumulated = (char *)os_alloc(sp.accum_cap);
    sp.model = m;
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

    if (sp.accum_len == 0) {
        LOG_WARN("LLM: no content (total=%d), fallback to non-stream", total);
        os_free(sp.accumulated);
        return llm_chat(endpoint, api_key, model, prompt);
    }

    llm_response_t *resp = (llm_response_t *)os_calloc(1, sizeof(*resp));
    if (!resp) { os_free(sp.accumulated); return NULL; }
    resp->content = sp.accumulated;
    resp->content_len = sp.accum_len;
    resp->prompt_tokens = sp.prompt_tokens;
    resp->completion_tokens = sp.completion_tokens;
    if (resp->completion_tokens == 0 && sp.tokens > 0)
        resp->completion_tokens = sp.tokens;

    LOG_DEBUG("LLM: stream done (%zu chars, %d tok)",
             resp->content_len, resp->completion_tokens);
    return resp;
}

/* ── Module Registration ──────────────────────────────────────────── */

static int llm_client_init(framework_module_t *mod)
{
    g_ctx = (llm_client_ctx_t *)os_calloc(1, sizeof(llm_client_ctx_t));
    if (!g_ctx) return -1;
    mod->ctx = g_ctx;
    llm_model_discover();
    LOG_INFO("LLM: init"); return 0;
}
static int llm_client_start(framework_module_t *mod)
{
    (void)mod; LOG_INFO("LLM: ready"); return 0;
}
static int llm_client_stop(framework_module_t *mod)
{
    (void)mod; LOG_INFO("LLM: stop"); return 0;
}
    framework_module_t llm_client_mod = {
    .name = "llm_client", .version = 0x00020000, 
    .state = FRAMEWORK_STATE_UNLOADED, .init = llm_client_init,
    .start = llm_client_start, .loop = NULL,
    .stop = llm_client_stop, .deinit = NULL,
    .ctx = NULL, .id = 0, .next = NULL,
};
MODULE_REGISTER(llm_client_mod);
