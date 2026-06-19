/*
 * tool_manager.c — Tool registry with thread-safe registration and dispatch.
 *
 * Tools are stored in a fixed-size array protected by a mutex.
 * tool_list_json() produces a JSON array suitable for LLM function calling.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "tool_manager.h"

#define MAX_TOOLS 64

typedef struct {
    tool_def_t def;
    int        active;
} tool_entry_t;

static tool_entry_t       g_tools[MAX_TOOLS];
static int                g_tool_count;
static os_mutex_handle_t  g_tool_mutex;

/* ── Helpers ──────────────────────────────────────────────────────── */

static int json_append(char *buf, size_t buf_len, size_t *pos, const char *s)
{
    size_t slen = os_strlen(s);
    if (*pos + slen >= buf_len) return -1;
    os_memcpy(buf + *pos, s, slen);
    *pos += slen;
    return 0;
}

static int json_append_escaped(char *buf, size_t buf_len, size_t *pos, const char *s)
{
    for (const char *c = s; *c; c++) {
        if (*pos + 2 >= buf_len) return -1;
        if (*c == '"' || *c == '\\') {
            buf[(*pos)++] = '\\';
        }
        buf[(*pos)++] = *c;
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

int tool_register(const tool_def_t *tool)
{
    if (!tool || !tool->name || !tool->execute) return -1;

    os_mutex_lock(g_tool_mutex);

    /* Check for duplicate */
    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].active &&
            os_strcmp(g_tools[i].def.name, tool->name) == 0) {
            os_mutex_unlock(g_tool_mutex);
            LOG_WARN("ToolManager: duplicate '%s'", tool->name);
            return -1;
        }
    }

    if (g_tool_count >= MAX_TOOLS) {
        os_mutex_unlock(g_tool_mutex);
        LOG_ERROR("ToolManager: registry full (%d)", MAX_TOOLS);
        return -1;
    }

    g_tools[g_tool_count].def = *tool;
    g_tools[g_tool_count].active = 1;
    g_tool_count++;

    LOG_INFO("ToolManager: registered '%s'", tool->name);
    os_mutex_unlock(g_tool_mutex);
    return 0;
}

int tool_call(const char *name, const char *args_json,
              char *result, size_t result_len)
{
    if (!name || !result || result_len == 0) return -1;
    if (!args_json) args_json = "{}";

    os_mutex_lock(g_tool_mutex);

    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].active &&
            os_strcmp(g_tools[i].def.name, name) == 0) {
            tool_def_t *t = &g_tools[i].def;
            LOG_INFO("ToolManager: calling '%s'", name);
            os_mutex_unlock(g_tool_mutex);

            int rc = t->execute(args_json, result, result_len, NULL);
            return rc;
        }
    }

    os_mutex_unlock(g_tool_mutex);
    LOG_WARN("ToolManager: tool '%s' not found", name);
    return -1;
}

int tool_list_json(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 3) return -1;

    size_t pos = 0;
    buf[0] = '\0';

    os_mutex_lock(g_tool_mutex);

    if (json_append(buf, buf_len, &pos, "[") != 0) {
        os_mutex_unlock(g_tool_mutex);
        return -1;
    }

    int count = 0;
    for (int i = 0; i < g_tool_count; i++) {
        if (!g_tools[i].active) continue;

        if (count > 0 && json_append(buf, buf_len, &pos, ",") != 0)
            break;

        if (json_append(buf, buf_len, &pos,
                        "{\"name\":\"") != 0) break;
        if (json_append_escaped(buf, buf_len, &pos,
                                g_tools[i].def.name) != 0) break;
        if (json_append(buf, buf_len, &pos,
                        "\",\"description\":\"") != 0) break;
        if (json_append_escaped(buf, buf_len, &pos,
                                g_tools[i].def.description) != 0) break;
        if (json_append(buf, buf_len, &pos, "\",\"parameters\":") != 0) break;
        if (json_append(buf, buf_len, &pos,
                        g_tools[i].def.params_json) != 0) break;
        if (json_append(buf, buf_len, &pos, "}") != 0) break;

        count++;
    }

    json_append(buf, buf_len, &pos, "]");

    os_mutex_unlock(g_tool_mutex);
    return count;
}

int tool_count(void)
{
    os_mutex_lock(g_tool_mutex);
    int n = g_tool_count;
    os_mutex_unlock(g_tool_mutex);
    return n;
}

/* ── Module Registration ──────────────────────────────────────────── */

static int echo_execute(const char *args_json, char *result,
                        size_t result_len, void *user_data)
{
    (void)user_data;
    return os_snprintf(result, result_len,
                       "{\"echo\":%s}", args_json ? args_json : "{}");
}

static int tool_manager_init(framework_module_t *mod)
{
    (void)mod;

    g_tool_mutex = os_mutex_create();
    if (!g_tool_mutex) {
        LOG_ERROR("ToolManager: mutex create failed");
        return -1;
    }

    g_tool_count = 0;
    for (int i = 0; i < MAX_TOOLS; i++) {
        g_tools[i].active = 0;
    }

    LOG_INFO("ToolManager: init (%d slots)", MAX_TOOLS);
    return 0;
}

static int tool_manager_start(framework_module_t *mod)
{
    (void)mod;

    /* Register built-in demo tool */
    tool_def_t echo = {
        .name = "echo",
        .description = "Echo back the input arguments",
        .params_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = echo_execute,
    };
    tool_register(&echo);

    LOG_INFO("ToolManager: ready (%d tool%s)",
             g_tool_count, g_tool_count == 1 ? "" : "s");
    return 0;
}

static int tool_manager_stop(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("ToolManager: stop");
    return 0;
}

static int tool_manager_deinit(framework_module_t *mod)
{
    (void)mod;
    if (g_tool_mutex) {
        os_mutex_destroy(g_tool_mutex);
        g_tool_mutex = NULL;
    }
    LOG_INFO("ToolManager: deinit");
    return 0;
}

framework_module_t tool_manager_mod = {
    .name     = "tool_manager",
    .version  = 0x00010000,
    .priority = 380,
    .state    = FRAMEWORK_STATE_UNLOADED,
    .init     = tool_manager_init,
    .start    = tool_manager_start,
    .loop     = NULL,
    .stop     = tool_manager_stop,
    .deinit   = tool_manager_deinit,
    .ctx      = NULL,
    .id       = 0,
    .next     = NULL,
};

MODULE_REGISTER(tool_manager_mod);
