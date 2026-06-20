/* model_agnes.c — Agnes API model adapter
 *
 * Agnes API (apihub.agnes-ai.com/v1) is OpenAI-compatible.
 * Standard JSON response with "content" field and usage stats.
 * Endpoint expects path /v1/chat/completions.
 */

#include "llm_core.h"
#include "os_api.h"
#include <string.h>

#define BUF_SIZE 65536

/* ── Build request body ─────────────────────────────────────────── */

static int agnes_build_body(char *buf, size_t buf_len,
                             const char *model, const char *prompt,
                             int stream)
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
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"stream\":%s"
        "}",
        model, safe_prompt, stream ? "true" : "false");
}

/* ── Extract content ────────────────────────────────────────────── */
/* Standard OpenAI: just "content" field */

static char* agnes_extract_content(const char *json, size_t *out_len,
                                   int *out_is_reasoning)
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
    if (len == 0) return NULL;

    char *content = (char *)os_alloc(len + 1);
    if (!content) return NULL;
    os_memcpy(content, start, len);
    content[len] = '\0';
    if (out_len) *out_len = len;
    return content;
}

/* ── Extract integer field ──────────────────────────────────────── */

static int agnes_extract_int(const char *json, const char *field)
{
    char key[64];
    int kn = os_snprintf(key, sizeof(key), "\"%s\":", field);
    if (kn <= 0) return 0;

    const char *p = json;
    while (*p) {
        const char *k = key, *s = p;
        while (*k && *s && *k == *s) { s++; k++; }
        if (!*k) {
            while (*s == ' ' || *s == '\t') s++;
            int n = 0;
            while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
            return n;
        }
        p++;
    }
    return 0;
}

/* ── Adapter definition ─────────────────────────────────────────── */

const llm_model_t llm_model_agnes = {
    .name            = "agnes-",
    .build_body      = agnes_build_body,
    .extract_content = agnes_extract_content,
    .extract_int     = agnes_extract_int,
};

/* Self-register via .llm_models section */
LLM_MODEL_REGISTER(llm_model_agnes);
