/*
 * tool_executor.c — Parse, validate, execute, format tool calls
 *
 * Handles the <tool_call>...</tool_call> format with JSON payload.
 * Supports both double-quote and single-quote JSON for robustness.
 */

#include "tool_executor.h"
#include "tool_manager.h"
#include "config.h"
#include "os_api.h"
#include "agent_framework.h"
#include <string.h>

/*
 * Normalize JSON: replace single quotes with double quotes,
 * but only for JSON structural delimiters (not literal ' inside values).
 *
 * Strategy:
 *   - Outside a string: ' is an opening JSON delimiter → replace with "
 *   - Inside a string: check if ' is followed by , } ] or : (closing delimiter)
 *     → yes: replace with " (exit string)
 *     → no:  literal ' inside value → leave as-is
 *   - Handles escaped chars: \\, \', \", etc. — skip past them
 *
 * Examples:
 *   {'name':'bash'}           → {"name":"bash"}        (key+value quoted)
 *   {'cmd':'echo 'hello''}    → {"cmd":"echo 'hello'"} (literal ' preserved)
 */
static void normalize_json(char *buf)
{
    if (!buf || !*buf) return;
    int in_str = 0;
    int replacements = 0;
    LOG_DEBUG("ToolExec: normalize input (first 100): %.*s", 100, buf);
    for (char *p = buf; *p; p++) {
        if (*p == '\\' && *(p + 1)) {
            p++;  /* skip escaped character (\\, \', \", etc.) */
            continue;
        }
        if (*p == '"') {
            in_str = !in_str;
        } else if (*p == '\'' && !in_str) {
            /* Outside string: ' is an opening JSON delimiter */
            *p = '"';
            in_str = 1;
            replacements++;
        } else if (*p == '\'' && in_str) {
            /* Inside string: check if ' is structural closing delimiter */
            const char *next = p + 1;
            while (*next == ' ' || *next == '\t') next++;
            if (*next == ',' || *next == '}' || *next == ']' || *next == ':') {
                *p = '"';
                in_str = 0;
                replacements++;
            }
            /* else: literal ' inside value (e.g. '*.git'), leave as-is */
        }
    }
    LOG_DEBUG("ToolExec: normalize done (%d replacements)", replacements);
}

/*
 * Search for a JSON key (double-quoted) in a buffer, returning the start
 * of its value. Handles both "key" and 'key' forms (after normalization).
 */
static const char *json_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    size_t klen = os_strlen(key);

    /* Build search pattern: "key": */
    /* Try both with and without trailing quote+colon */
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* Check if preceded by { , or whitespace (also " for {"name":...}) */
        if (p > json) {
            char prev = *(p - 1);
            if (prev != '{' && prev != ',' && prev != ' ' && prev != '\t' && prev != '\n' && prev != '"') {
                p++;
                continue;
            }
        }
        /* After key, expect " or ' then : */
        const char *after = p + klen;
        while (*after == ' ' || *after == '\t') after++;
        if (*after == ':' || *after == '"' || *after == '\'') {
            /* Skip to actual value after key" : or key' : */
            const char *colon = after;
            if (*after == '"' || *after == '\'') colon = after + 1;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (*colon == ':') {
                const char *val = colon + 1;
                while (*val == ' ' || *val == '\t') val++;
                return val;
            }
        }
        p++;
    }
    return NULL;
}

tool_parse_status_t tool_parse_call(const char *response, tool_call_t *call)
{
    if (!response || !call) return TOOL_PARSE_INVALID;
    os_memset(call, 0, sizeof(*call));

    LOG_DEBUG("ToolExec: parse_call input (first 200): %.*s",
              200, response);

    /* Find <tool_call>...</tool_call> */
    const char *open = strstr(response, "<tool_call>");
    if (!open) {
        LOG_DEBUG("ToolExec: parse_call — no <tool_call> tag found");
        return TOOL_PARSE_NONE;
    }
    LOG_DEBUG("ToolExec: parse_call — found <tool_call> at offset %td", open - response);

    const char *json_start = open + 11;  /* skip <tool_call> */
    while (*json_start == '\n' || *json_start == '\r' || *json_start == ' ') json_start++;

    const char *close = strstr(json_start, "</tool_call>");
    if (!close) {
        LOG_DEBUG("ToolExec: parse_call — no </tool_call> tag found");
        return TOOL_PARSE_INVALID;
    }

    /* Extract JSON block between tags */
    size_t json_len = (size_t)(close - json_start);
    LOG_DEBUG("ToolExec: parse_call — json block len=%zu", json_len);
    char json_buf[2048];
    size_t cp = json_len < sizeof(json_buf) - 1 ? json_len : sizeof(json_buf) - 1;
    os_memcpy(json_buf, json_start, cp);
    json_buf[cp] = '\0';

    /* Normalize single quotes to double quotes for robust parsing */
    normalize_json(json_buf);

    /* Parse name field */
    const char *name_v = json_find_key(json_buf, "name");
    if (!name_v) {
        LOG_DEBUG("ToolExec: parse_call — 'name' key not found");
        return TOOL_PARSE_INVALID;
    }

    /* Skip opening quote */
    while (*name_v == ' ' || *name_v == '"' || *name_v == '\'') name_v++;
    const char *name_end = strchr(name_v, '"');
    if (!name_end) {
        /* Try single quote */
        name_end = strchr(name_v, '\'');
    }
    if (!name_end) {
        LOG_DEBUG("ToolExec: parse_call — cannot find name string end");
        return TOOL_PARSE_INVALID;
    }
    size_t nlen = (size_t)(name_end - name_v);
    if (nlen >= sizeof(call->name)) nlen = sizeof(call->name) - 1;
    os_memcpy(call->name, name_v, nlen);
    call->name[nlen] = '\0';
    LOG_DEBUG("ToolExec: parse_call — name='%s'", call->name);

    /* Parse arguments field: "arguments":{...} */
    const char *arg_k = json_find_key(json_buf, "arguments");
    if (!arg_k) {
        LOG_DEBUG("ToolExec: parse_call — 'arguments' key not found");
        return TOOL_PARSE_INVALID;
    }
    const char *arg_v = arg_k;
    while (*arg_v && *arg_v != '{') arg_v++;
    if (!*arg_v) {
        LOG_DEBUG("ToolExec: parse_call — no arguments object");
        return TOOL_PARSE_INVALID;
    }

    /* Find matching closing brace */
    int brace_depth = 0;
    const char *arg_end = NULL;
    for (const char *c = arg_v; *c; c++) {
        if (*c == '{') brace_depth++;
        if (*c == '}') {
            brace_depth--;
            if (brace_depth == 0) { arg_end = c; break; }
        }
    }
    if (!arg_end) {
        LOG_DEBUG("ToolExec: parse_call — unclosed arguments brace");
        return TOOL_PARSE_INVALID;
    }

    size_t alen = (size_t)(arg_end - arg_v + 1);
    size_t acp = alen < sizeof(call->args_json) - 1 ? alen : sizeof(call->args_json) - 1;
    os_memcpy(call->args_json, arg_v, acp);
    call->args_json[acp] = '\0';
    LOG_DEBUG("ToolExec: parse_call — args (first 200): %.*s",
              (int)(os_strlen(call->args_json) < 200 ? os_strlen(call->args_json) : 200),
              call->args_json);

    return TOOL_PARSE_OK;
}

int tool_execute_call(const tool_call_t *call, char *result, size_t result_len)
{
    if (!call || !result || result_len < 10) return -1;
    result[0] = '\0';

    LOG_DEBUG("ToolExec: execute_call tool='%s' args='%s'",
              call->name, call->args_json);

    /* Visual tool indicator on stderr (debug UI) */
    os_fprintf_stderr("\n  \033[2m⚙ %s\033[0m\n", call->name);

    /* 1. Validate tool exists */
    tool_info_t info;
    if (tool_find(call->name, &info) != 0) {
        LOG_DEBUG("ToolExec: execute_call — tool '%s' not found", call->name);
        /* List available tools */
        os_snprintf(result, result_len,
            "<tool_result>\n"
            "tool: %s\n"
            "status: error\n"
            "message: tool not found\n"
            "available: ",
            call->name);
        int n = tool_count();
        size_t pos = os_strlen(result);
        for (int i = 0; i < n && pos < result_len - 20; i++) {
            tool_info_t ti;
            if (tool_get_info(i, &ti) != 0) continue;
            if (i > 0 && pos < result_len - 2) result[pos++] = ' ';
            size_t nlen = os_strlen(ti.name);
            size_t cp = nlen < result_len - pos - 2 ? nlen : result_len - pos - 2;
            os_memcpy(result + pos, ti.name, cp);
            pos += cp;
        }
        pos += os_snprintf(result + pos, result_len - pos, "\n</tool_result>\n");
        return -1;
    }

    LOG_DEBUG("ToolExec: execute_call — tool risk=%d (shell=2), shell_confirm=%d",
              info.risk, config_get() ? config_get()->shell_confirm : 1);

    /* 2. Risk check — optionally require user confirmation for shell/dangerous tools */
    if (info.risk >= TOOL_RISK_SHELL && config_get() && config_get()->shell_confirm) {
        char confirm_prompt[512];
        os_snprintf(confirm_prompt, sizeof(confirm_prompt),
                    "[!] Tool '%s' — %s\n    Args: %s\n    Execute?",
                    info.name, info.description ? info.description : "",
                    call->args_json);
        if (!os_confirm(confirm_prompt)) {
            LOG_DEBUG("ToolExec: execute_call — user rejected '%s'", call->name);
            os_snprintf(result, result_len,
                "<tool_result>\n"
                "tool: %s\n"
                "status: cancelled\n"
                "message: rejected by user\n"
                "</tool_result>\n",
                call->name);
            return 0;
        }
        LOG_DEBUG("ToolExec: execute_call — user approved '%s'", call->name);
    }

    /* 3. Validate arguments */
    char err[256];
    if (tool_validate(call->name, call->args_json, err, sizeof(err)) != 0) {
        LOG_DEBUG("ToolExec: execute_call — validation failed: %s", err);
        os_snprintf(result, result_len,
            "<tool_result>\n"
            "tool: %s\n"
            "status: error\n"
            "message: %s\n"
            "</tool_result>\n",
            call->name, err);
        return -1;
    }

    /* 4. Execute */
    char tool_output[TOOL_RESULT_MAX];
    LOG_DEBUG("ToolExec: execute_call — dispatching to tool_call()");
    int rc = tool_call(call->name, call->args_json, tool_output, sizeof(tool_output));
    LOG_DEBUG("ToolExec: execute_call — tool_call() rc=%d, output_len=%zu",
              rc, tool_output[0] ? os_strlen(tool_output) : 0);
    if (rc < 0) {
        LOG_DEBUG("ToolExec: execute_call — execution failed (rc=%d)", rc);
        os_snprintf(result, result_len,
            "<tool_result>\n"
            "tool: %s\n"
            "status: error\n"
            "message: execution failed\n"
            "</tool_result>\n",
            call->name);
        return -1;
    }

    /* 5. Format success result with truncation guard */
    size_t out_len = os_strlen(tool_output);
    size_t max_content = result_len - 256;  /* reserve room for XML wrapper */
    int truncated = (out_len > max_content);

    if (truncated) {
        LOG_DEBUG("ToolExec: execute_call — truncated %zu -> %zu bytes",
                  out_len, max_content);
    }

    size_t copy_len = truncated ? max_content : out_len;
    size_t pos = 0;
    pos += os_snprintf(result + pos, result_len - pos,
        "<tool_result>\n"
        "tool: %s\n"
        "status: success\n"
        "content:\n", call->name);
    /* Copy only what fits */
    if (copy_len > 0) {
        os_memcpy(result + pos, tool_output, copy_len);
        pos += copy_len;
    }
    if (truncated) {
        pos += os_snprintf(result + pos, result_len - pos,
            "\n... (truncated, %zu more bytes)", out_len - copy_len);
    }
    os_snprintf(result + pos, result_len - pos,
        "\n</tool_result>\n");

    return 0;
}
