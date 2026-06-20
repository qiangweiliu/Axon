/*
 * tool_schema.c — Auto-generate tool descriptions from tool_manager
 */

#include "tool_schema.h"
#include "tool_manager.h"
#include "os_api.h"

int tool_schema_build(char *buf, size_t len)
{
    if (!buf || len < 10) return 0;
    int pos = 0;

    pos += os_snprintf(buf + pos, len - pos,
        "You can use tools when needed.\n\n"
        "Format for tool calls:\n"
        "<tool_call>\n"
        "{\"name\":\"tool\",\"arguments\":{...}}\n"
        "</tool_call>\n\n"
        "Rules:\n"
        "- Only output ONE tool_call block at a time.\n"
        "- Wait for the result before calling another tool.\n"
        "- If you have enough information, do NOT call a tool.\n"
        "- NEVER say you cannot access the file system. Use tools.\n\n"
        "Available tools:\n");

    int n = tool_count();
    for (int i = 0; i < n && pos < (int)len - 200; i++) {
        tool_info_t info;
        if (tool_get_info(i, &info) != 0) continue;
        if (!info.enabled) continue;

        pos += os_snprintf(buf + pos, len - pos,
            "  %s\n    %s\n", info.name, info.description);

        /* Format params_json into readable args text */
        if (info.params_json) {
            /* Extract property names from JSON schema for display */
            const char *props = strstr(info.params_json, "\"properties\"");
            if (props) {
                pos += os_snprintf(buf + pos, len - pos, "    Args: ");
                const char *p = props;
                int first = 1;
                while (*p && pos < (int)len - 50) {
                    /* Find "name": */
                    const char *key = strstr(p, "\"description\":\"");
                    if (key) {
                        key += 14;
                        const char *kend = strchr(key, '"');
                        if (kend) {
                            if (!first) pos += os_snprintf(buf + pos, len - pos, ", ");
                            first = 0;
                            size_t klen = (size_t)(kend - key);
                            if (klen > 60) klen = 60;
                            pos += os_snprintf(buf + pos, len - pos, "%.*s", (int)klen, key);
                            p = kend;
                            continue;
                        }
                    }
                    break;
                }
                pos += os_snprintf(buf + pos, len - pos, "\n");
            }
        }
    }

    pos += os_snprintf(buf + pos, len - pos, "\n");
    return pos;
}
