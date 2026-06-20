/*
 * skill_tools.c — Register skills as tools in the tool_manager
 *
 * Registers two tools:
 *   list_skills — returns JSON array of available skills
 *   load_skill  — load a skill's body content by name
 *
 * Called from skill_manager_start() after skill index is built.
 */

#include "agent_framework.h"
#include "os_api.h"
#include "skill_manager.h"
#include "tool_manager.h"
#include <string.h>

/* ── list_skills: JSON list of all available skills ───────────────── */

static int list_skills_execute(const char *args_json, char *result,
                                size_t result_len, void *user_data)
{
    (void)args_json;
    (void)user_data;
    if (result_len < 4) return -1;

    int n = skill_count();
    int pos = 0;
    pos += os_snprintf(result + pos, result_len - pos, "{\"skills\":[");
    for (int i = 0; i < n && pos < (int)result_len - 100; i++) {
        const skill_entry_t *e = skill_get(i);
        if (!e) continue;
        if (i > 0) pos += os_snprintf(result + pos, result_len - pos, ",");
        pos += os_snprintf(result + pos, result_len - pos,
                           "{\"name\":\"%s\",\"description\":\"%s\",\"category\":\"%s\"}",
                           e->name, e->description, e->category);
    }
    pos += os_snprintf(result + pos, result_len - pos, "]}");
    return (size_t)pos < result_len ? 0 : -1;
}

/* ── load_skill: load a skill's body content ──────────────────────── */

static int load_skill_execute(const char *args_json, char *result,
                               size_t result_len, void *user_data)
{
    (void)user_data;
    if (result_len < 4) return -1;
    result[0] = '\0';

    /* Parse name from JSON: {"name":"skill-name"} */
    const char *name_start = strstr(args_json ? args_json : "", "\"name\":\"");
    if (!name_start) {
        return os_snprintf(result, result_len, "{\"error\":\"missing name argument\"}");
    }
    name_start += 8;
    char name[64];
    size_t ni = 0;
    while (name_start[ni] && name_start[ni] != '"' && ni < sizeof(name) - 1) {
        name[ni] = name_start[ni];
        ni++;
    }
    name[ni] = '\0';

    char *content = skill_load(name);
    if (!content) {
        return os_snprintf(result, result_len,
                           "{\"error\":\"skill not found\",\"name\":\"%s\"}", name);
    }

    /* Escape content for JSON */
    int pos = os_snprintf(result, result_len, "{\"name\":\"%s\",\"content\":\"", name);
    for (char *p = content; *p && pos < (int)result_len - 10; p++) {
        if (*p == '"') {
            if (pos < (int)result_len - 4) { result[pos++] = '\\'; result[pos++] = '"'; }
        } else if (*p == '\\') {
            if (pos < (int)result_len - 4) { result[pos++] = '\\'; result[pos++] = '\\'; }
        } else if (*p == '\n') {
            if (pos < (int)result_len - 4) { result[pos++] = '\\'; result[pos++] = 'n'; }
        } else if (*p == '\t') {
            if (pos < (int)result_len - 4) { result[pos++] = '\\'; result[pos++] = 't'; }
        } else {
            if (pos < (int)result_len - 3) result[pos++] = *p;
        }
    }
    skill_free_content(content);

    if (pos < (int)result_len - 4) {
        os_snprintf(result + pos, result_len - pos, "\"}");
    }
    return 0;
}

/* ── Register skill tools ────────────────────────────────────────── */

int skill_tools_register(void)
{
    tool_def_t list_skills = {
        .name = "list_skills",
        .description = "List all available skills with their name, description and category",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "}}",
        .risk = TOOL_RISK_SAFE,
        .execute = list_skills_execute,
    };
    if (tool_register(&list_skills) != 0) {
        LOG_WARN("SkillTools: failed to register list_skills");
    }

    tool_def_t load_skill = {
        .name = "load_skill",
        .description = "Load a skill's full instruction content by name. "
                       "Use this when you need to follow a specific skill's instructions. "
                       "After loading, follow the skill's guidance carefully.",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"skill name to load\"}"
            "},\"required\":[\"name\"]}",
        .risk = TOOL_RISK_SAFE,
        .execute = load_skill_execute,
    };
    if (tool_register(&load_skill) != 0) {
        LOG_WARN("SkillTools: failed to register load_skill");
    }

    LOG_INFO("SkillTools: list_skills and load_skill registered");
    return 0;
}
