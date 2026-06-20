/*
 * tool_dispatch.c — Parse and execute [TOOL:] directives.
 *
 * Single responsibility: parse "name | args=<json>" from directive text,
 * call tool_manager to execute, return formatted result.
 * No knowledge of prompt building, Phase 2, or extra_buf.
 */

#include "tool_dispatch.h"
#include "tool_manager.h"
#include "os_api.h"
#include <string.h>

#define RESULT_MAX 4096

int tool_dispatch(const char *text, char *result, size_t result_len)
{
    if (!text || !result || result_len < 2) return -1;

    /* Parse: name | args=... */
    char tool_name[64];
    char tool_args[1024] = "{}";

    const char *pipe = strstr(text, " | ");
    if (pipe) {
        /* Extract tool name (text before " | ") */
        size_t nlen = (size_t)(pipe - text);
        if (nlen >= sizeof(tool_name)) nlen = sizeof(tool_name) - 1;
        os_memcpy(tool_name, text, nlen);
        tool_name[nlen] = '\0';

        /* Extract args (text after " | args=") */
        const char *args_start = pipe + 3;
        if (strstr(args_start, "args="))
            args_start += 5;
        os_strncpy(tool_args, args_start, sizeof(tool_args) - 1);
    } else {
        /* Whole text is the tool name, no args */
        os_strncpy(tool_name, text, sizeof(tool_name) - 1);
    }

    /* Call tool_manager */
    char tool_result[RESULT_MAX];
    int rc = tool_call(tool_name, tool_args, tool_result, sizeof(tool_result));
    if (rc < 0) {
        os_snprintf(result, result_len, "Tool '%s' not found", tool_name);
        return -1;
    }

    /* Copy result to output buffer */
    size_t rlen = os_strlen(tool_result);
    if (rlen >= result_len) rlen = result_len - 1;
    os_memcpy(result, tool_result, rlen);
    result[rlen] = '\0';
    return 0;
}
