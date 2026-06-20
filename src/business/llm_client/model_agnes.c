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
    size_t pi = 0;
    const char *p = prompt;
    while (*p && pi < sizeof(safe_prompt) - 6) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            safe_prompt[pi++] = '\'';
        } else if (c == '\\') {
            safe_prompt[pi++] = '\\';
            safe_prompt[pi++] = '\\';
        } else if (c == '\n') {
            safe_prompt[pi++] = '\\';
            safe_prompt[pi++] = 'n';
        } else if (c == '\r') {
            safe_prompt[pi++] = '\\';
            safe_prompt[pi++] = 'r';
        } else if (c == '\t') {
            safe_prompt[pi++] = '\\';
            safe_prompt[pi++] = 't';
        } else {
            safe_prompt[pi++] = c;
        }
        p++;
    }
    safe_prompt[pi] = '\0';

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

    /* Find end of JSON string value, handling \" escapes */
    const char *end = start;
    while (*end) {
        if (*end == '\\' && *(end + 1) == '"') {
            end += 2;  /* skip escaped quote */
        } else if (*end == '"') {
            break;  /* unescaped " — end of string */
        } else if (*end == '\\' && *(end + 1) == '\\') {
            end += 2;  /* skip escaped backslash */
        } else {
            end++;
        }
    }

    size_t len = (size_t)(end - start);
    if (len == 0) return NULL;

    /* Allocate and unescape JSON string */
    char *content = (char *)os_alloc(len + 1);
    if (!content) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            if (start[i + 1] == '"')  { content[o++] = '"'; i++; }
            else if (start[i + 1] == '\\') { content[o++] = '\\'; i++; }
            else if (start[i + 1] == 'n')  { content[o++] = '\n'; i++; }
            else if (start[i + 1] == 't')  { content[o++] = '\t'; i++; }
            else if (start[i + 1] == 'r')  { content[o++] = '\r'; i++; }
            else { content[o++] = start[i]; }
        } else {
            content[o++] = start[i];
        }
    }
    content[o] = '\0';
    if (out_len) *out_len = o;
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
