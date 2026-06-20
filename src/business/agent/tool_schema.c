/*
 * tool_schema.c — Auto-generate tool descriptions from tool_manager
 *
 * Generates a structured text block showing each tool's name,
 * description, and parameters (name, type, description from JSON schema).
 */

#include "tool_schema.h"
#include "tool_manager.h"
#include "os_api.h"
#include "agent_framework.h"
#include <string.h>

/*
 * Extract parameter info from a JSON properties entry like:
 *   "path":{"type":"string","description":"file path"}
 * Returns 1 on success, 0 if no more params found.
 * On success, fills name/type/desc and advances *pp past this entry.
 */
static int extract_param(const char **pp, char *name, size_t name_sz,
                         char *type, size_t type_sz,
                         char *desc, size_t desc_sz)
{
    const char *p = *pp;
    if (!p || !*p) return 0;

    /* Skip whitespace and commas */
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n') p++;

    /* Find property key: "key":{ */
    if (*p != '"') return 0;
    p++; /* skip opening " */
    const char *kstart = p;
    while (*p && *p != '"') p++;
    if (!*p) return 0;
    size_t klen = (size_t)(p - kstart);
    if (klen >= name_sz) klen = name_sz - 1;
    os_memcpy(name, kstart, klen);
    name[klen] = '\0';
    p++; /* skip closing " */

    /* Skip :{ */
    while (*p && *p != '{') { if (*p == ':') break; p++; }
    if (*p != ':') return 0;
    p++;
    while (*p && *p != '{') p++;
    if (*p != '{') return 0;
    p++; /* skip { */

    /* Parse key-value pairs inside the object */
    int got_type = 0, got_desc = 0;
    while (*p && *p != '}') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
        if (*p == '}') break;

        /* Read key */
        if (*p != '"') { p++; continue; }
        p++;
        const char *vk = p;
        while (*p && *p != '"') p++;
        if (!*p) return 0;
        size_t vkl = (size_t)(p - vk);
        p++; /* skip closing " */

        /* Skip : */
        while (*p && *p != ':' && *p != '}') p++;
        if (*p != ':') { p++; continue; }
        p++;
        while (*p == ' ' || *p == '\t') p++;

        if (vkl == 4 && os_strncmp(vk, "type", 4) == 0 && !got_type) {
            /* Read string value */
            if (*p == '"') {
                p++;
                const char *vs = p;
                while (*p && *p != '"') p++;
                size_t vsl = (size_t)(p - vs);
                if (vsl >= type_sz) vsl = type_sz - 1;
                os_memcpy(type, vs, vsl);
                type[vsl] = '\0';
                got_type = 1;
                if (*p) p++;
            }
        } else if ((vkl == 11 && os_strncmp(vk, "description", 11) == 0) && !got_desc) {
            if (*p == '"') {
                p++;
                const char *vs = p;
                while (*p && *p != '"') p++;
                size_t vsl = (size_t)(p - vs);
                if (vsl >= desc_sz) vsl = desc_sz - 1;
                os_memcpy(desc, vs, vsl);
                desc[vsl] = '\0';
                got_desc = 1;
                if (*p) p++;
            }
        } else {
            /* Skip unknown value */
            if (*p == '"') {
                p++; while (*p && *p != '"') p++; if (*p) p++;
            } else if (*p == '{') {
                int bd = 1; p++;
                while (*p && bd > 0) { if (*p == '{') bd++; if (*p == '}') bd--; p++; }
            } else {
                while (*p && *p != ',' && *p != '}' && *p != '\n') p++;
            }
        }
    }

    /* Advance past closing } */
    if (*p == '}') p++;
    *pp = p;

    if (!name[0]) return 0;
    LOG_DEBUG("ToolSchema: extract_param — name='%s' type='%s' desc='%s'",
              name, type, desc);
    return 1;
}

int tool_schema_build(char *buf, size_t len)
{
    if (!buf || len < 10) return 0;
    int pos = 0;

    pos += os_snprintf(buf + pos, len - pos,
        "You can use tools when needed.\n\n"
        "Format for tool calls:\n"
        "<tool_call>\n"
        "{\"name\":\"tool_name\",\"arguments\":{\"arg1\":\"value1\",\"arg2\":\"value2\"}}\n"
        "</tool_call>\n\n"
        "Rules:\n"
        "- Output ONE <tool_call> block at a time.\n"
        "- Wait for the result before calling another tool.\n"
        "- If you have enough information to answer, do NOT call a tool.\n"
        "- NEVER say you cannot access files/the system. Use tools.\n\n"
        "Available tools:\n");

    int n = tool_count();
    for (int i = 0; i < n && pos < (int)len - 200; i++) {
        tool_info_t info;
        if (tool_get_info(i, &info) != 0) continue;
        if (!info.enabled) continue;
        pos += os_snprintf(buf + pos, len - pos,
            "  %s\n    %s\n", info.name, info.description);

        /* Parse and display parameters from JSON schema */
        if (info.params_json && info.params_json[0]) {
            /* Find "properties":{...} */
            const char *props = strstr(info.params_json, "\"properties\"");
            if (props) {
                const char *p = strchr(props, '{');
                if (p) {
                    p++; /* skip first { after "properties" */
                    /* Find the actual object start */
                    while (*p && *p != '{') p++;
                    if (*p) {
                        p++; /* skip { */
                        pos += os_snprintf(buf + pos, len - pos, "    Args:\n");
                        char pname[64], ptype[32], pdesc[128];
                        while (extract_param(&p, pname, sizeof(pname),
                                             ptype, sizeof(ptype),
                                             pdesc, sizeof(pdesc))) {
                            if (!ptype[0]) os_strncpy(ptype, "string", 7);
                            pos += os_snprintf(buf + pos, len - pos,
                                "      %s (%s): %s\n",
                                pname, ptype, pdesc[0] ? pdesc : "-");
                        }
                    }
                }
            }
        }
    }

    pos += os_snprintf(buf + pos, len - pos, "\n");
    LOG_DEBUG("ToolSchema: build done, total=%d/%zu bytes", pos, len);
    return pos;
}
