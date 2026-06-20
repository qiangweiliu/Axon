/*
 * tool_executor.c — Parse, validate, execute, format tool calls
 */

#include "tool_executor.h"
#include "tool_manager.h"
#include "os_api.h"
#include <string.h>

tool_parse_status_t tool_parse_call(const char *response, tool_call_t *call)
{
    if (!response || !call) return TOOL_PARSE_INVALID;
    os_memset(call, 0, sizeof(*call));

    /* Find <tool_call>...</tool_call> */
    const char *open = strstr(response, "<tool_call>");
    if (!open) return TOOL_PARSE_NONE;

    const char *json_start = open + 11;  /* skip <tool_call> */
    while (*json_start == '\n' || *json_start == '\r' || *json_start == ' ') json_start++;

    const char *close = strstr(json_start, "</tool_call>");
    if (!close) return TOOL_PARSE_INVALID;

    /* Extract JSON block between tags */
    size_t json_len = (size_t)(close - json_start);
    char json_buf[2048];
    size_t cp = json_len < sizeof(json_buf) - 1 ? json_len : sizeof(json_buf) - 1;
    os_memcpy(json_buf, json_start, cp);
    json_buf[cp] = '\0';

    /* Parse name field: "name":"value" */
    const char *name_k = strstr(json_buf, "\"name\"");
    if (!name_k) return TOOL_PARSE_INVALID;
    const char *name_v = strchr(name_k, ':');
    if (!name_v) return TOOL_PARSE_INVALID;
    name_v++;
    while (*name_v == ' ' || *name_v == '"') name_v++;
    const char *name_end = strchr(name_v, '"');
    if (!name_end) return TOOL_PARSE_INVALID;
    size_t nlen = (size_t)(name_end - name_v);
    if (nlen >= sizeof(call->name)) nlen = sizeof(call->name) - 1;
    os_memcpy(call->name, name_v, nlen);
    call->name[nlen] = '\0';

    /* Parse arguments field: "arguments":{...} */
    const char *arg_k = strstr(json_buf, "\"arguments\"");
    if (!arg_k) return TOOL_PARSE_INVALID;
    const char *arg_v = strchr(arg_k, '{');
    if (!arg_v) return TOOL_PARSE_INVALID;

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
    if (!arg_end) return TOOL_PARSE_INVALID;

    size_t alen = (size_t)(arg_end - arg_v + 1);
    size_t acp = alen < sizeof(call->args_json) - 1 ? alen : sizeof(call->args_json) - 1;
    os_memcpy(call->args_json, arg_v, acp);
    call->args_json[acp] = '\0';

    return TOOL_PARSE_OK;
}

int tool_execute_call(const tool_call_t *call, char *result, size_t result_len)
{
    if (!call || !result || result_len < 10) return -1;
    result[0] = '\0';

    /* 1. Validate tool exists */
    tool_info_t info;
    if (tool_find(call->name, &info) != 0) {
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

    /* 2. Validate arguments */
    char err[256];
    if (tool_validate(call->name, call->args_json, err, sizeof(err)) != 0) {
        os_snprintf(result, result_len,
            "<tool_result>\n"
            "tool: %s\n"
            "status: error\n"
            "message: %s\n"
            "</tool_result>\n",
            call->name, err);
        return -1;
    }

    /* 3. Execute */
    char tool_output[TOOL_RESULT_MAX];
    int rc = tool_call(call->name, call->args_json, tool_output, sizeof(tool_output));
    if (rc < 0) {
        os_snprintf(result, result_len,
            "<tool_result>\n"
            "tool: %s\n"
            "status: error\n"
            "message: execution failed\n"
            "</tool_result>\n",
            call->name);
        return -1;
    }

    /* 4. Format success result */
    size_t out_len = os_strlen(tool_output);
    os_snprintf(result, result_len,
        "<tool_result>\n"
        "tool: %s\n"
        "status: success\n"
        "content:\n"
        "%.*s\n"
        "</tool_result>\n",
        call->name,
        (int)(out_len < result_len - 200 ? out_len : result_len - 200),
        tool_output);

    return 0;
}
