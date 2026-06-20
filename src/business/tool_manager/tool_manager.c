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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_TOOLS 64
typedef struct {
    tool_def_t def;
    int        active;
} tool_entry_t;

typedef struct {
    tool_entry_t       tools[MAX_TOOLS];
    int                tool_count;
    os_mutex_handle_t  tool_mutex;
} tool_manager_ctx_t;

static tool_manager_ctx_t *g_ctx = NULL;

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

    os_mutex_lock(g_ctx->tool_mutex);

    /* Check for duplicate */
    for (int i = 0; i < g_ctx->tool_count; i++) {
        if (g_ctx->tools[i].active &&
            os_strcmp(g_ctx->tools[i].def.name, tool->name) == 0) {
            os_mutex_unlock(g_ctx->tool_mutex);
            LOG_WARN("ToolManager: duplicate '%s'", tool->name);
            return -1;
        }
    }

    if (g_ctx->tool_count >= MAX_TOOLS) {
        os_mutex_unlock(g_ctx->tool_mutex);
        LOG_ERROR("ToolManager: registry full (%d)", MAX_TOOLS);
        return -1;
    }

    g_ctx->tools[g_ctx->tool_count].def = *tool;
    g_ctx->tools[g_ctx->tool_count].active = 1;
    g_ctx->tool_count++;

    LOG_INFO("ToolManager: registered '%s'", tool->name);
    os_mutex_unlock(g_ctx->tool_mutex);
    return 0;
}

int tool_call(const char *name, const char *args_json,
              char *result, size_t result_len)
{
    if (!name || !result || result_len == 0) return -1;
    if (!args_json) args_json = "{}";

    os_mutex_lock(g_ctx->tool_mutex);

    for (int i = 0; i < g_ctx->tool_count; i++) {
        if (g_ctx->tools[i].active &&
            os_strcmp(g_ctx->tools[i].def.name, name) == 0) {
            tool_def_t *t = &g_ctx->tools[i].def;
            LOG_INFO("ToolManager: calling '%s'", name);
            os_mutex_unlock(g_ctx->tool_mutex);

            int rc = t->execute(args_json, result, result_len, NULL);
            return rc;
        }
    }

    os_mutex_unlock(g_ctx->tool_mutex);
    LOG_WARN("ToolManager: tool '%s' not found", name);
    return -1;
}

int tool_list_json(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 3) return -1;

    size_t pos = 0;
    buf[0] = '\0';

    os_mutex_lock(g_ctx->tool_mutex);

    if (json_append(buf, buf_len, &pos, "[") != 0) {
        os_mutex_unlock(g_ctx->tool_mutex);
        return -1;
    }

    int count = 0;
    for (int i = 0; i < g_ctx->tool_count; i++) {
        if (!g_ctx->tools[i].active) continue;

        if (count > 0 && json_append(buf, buf_len, &pos, ",") != 0)
            break;

        if (json_append(buf, buf_len, &pos,
                        "{\"name\":\"") != 0) break;
        if (json_append_escaped(buf, buf_len, &pos,
                                g_ctx->tools[i].def.name) != 0) break;
        if (json_append(buf, buf_len, &pos,
                        "\",\"description\":\"") != 0) break;
        if (json_append_escaped(buf, buf_len, &pos,
                                g_ctx->tools[i].def.description) != 0) break;
        if (json_append(buf, buf_len, &pos, "\",\"parameters\":") != 0) break;
        if (json_append(buf, buf_len, &pos,
                        g_ctx->tools[i].def.params_json) != 0) break;
        if (json_append(buf, buf_len, &pos, "}") != 0) break;

        count++;
    }

    json_append(buf, buf_len, &pos, "]");

    os_mutex_unlock(g_ctx->tool_mutex);
    return count;
}

int tool_count(void)
{
    os_mutex_lock(g_ctx->tool_mutex);
    int n = g_ctx->tool_count;
    os_mutex_unlock(g_ctx->tool_mutex);
    return n;
}

int tool_get_info(int index, tool_info_t *info)
{
    if (!info) return -1;
    os_mutex_lock(g_ctx->tool_mutex);
    if (index < 0 || index >= g_ctx->tool_count) {
        os_mutex_unlock(g_ctx->tool_mutex);
        return -1;
    }
    tool_entry_t *e = &g_ctx->tools[index];
    info->name = e->def.name;
    info->description = e->def.description;
    info->params_json = e->def.params_json;
    info->risk = e->def.risk;
    info->enabled = e->active;
    os_mutex_unlock(g_ctx->tool_mutex);
    return 0;
}

int tool_find(const char *name, tool_info_t *info)
{
    if (!name || !info) return -1;
    os_mutex_lock(g_ctx->tool_mutex);
    for (int i = 0; i < g_ctx->tool_count; i++) {
        if (os_strcmp(g_ctx->tools[i].def.name, name) == 0) {
            tool_entry_t *e = &g_ctx->tools[i];
            info->name = e->def.name;
            info->description = e->def.description;
            info->params_json = e->def.params_json;
            info->risk = e->def.risk;
            info->enabled = e->active;
            os_mutex_unlock(g_ctx->tool_mutex);
            return 0;
        }
    }
    os_mutex_unlock(g_ctx->tool_mutex);
    return -1;
}

int tool_validate(const char *name, const char *args_json,
                  char *err, size_t err_len)
{
    if (!name || !args_json) return -1;

    /* Find tool's params_json schema */
    tool_info_t info;
    if (tool_find(name, &info) != 0) {
        os_snprintf(err, err_len, "unknown tool: %s", name);
        return -1;
    }
    if (!info.params_json || !*info.params_json)
        return 0;  /* no schema = no validation */

    /* Lightweight: check required fields exist in args_json */
    /* Parse schema for "required": [...] and check each key exists in args */
    const char *req = strstr(info.params_json, "\"required\"");
    if (!req) return 0;  /* no required fields */

    const char *arr_start = strchr(req, '[');
    if (!arr_start) return 0;
    arr_start++;

    const char *p = arr_start;
    while (*p && *p != ']') {
        /* Skip whitespace and quotes */
        while (*p == ' ' || *p == '"' || *p == ',') p++;
        if (*p == ']') break;

        /* Read field name */
        const char *fn_start = p;
        while (*p && *p != '"' && *p != ',' && *p != ']') p++;
        if (p == fn_start) break;

        size_t fn_len = (size_t)(p - fn_start);

        /* Check if args_json contains "fieldname": */
        /* Build search pattern: "fieldname": */
        char search[128];
        os_snprintf(search, sizeof(search), "\"%.*s\":", (int)fn_len, fn_start);

        if (!strstr(args_json, search) &&
            !strstr(args_json, "'") && !strstr(args_json, search)) {
            /* Also try single-quote variant */
            char search_sq[128];
            os_snprintf(search_sq, sizeof(search_sq), "'%.*s':", (int)fn_len, fn_start);
            if (!strstr(args_json, search_sq)) {
                os_snprintf(err, err_len, "missing required argument: %.*s",
                            (int)fn_len, fn_start);
                return -1;
            }
        }
        /* Skip past quote */
        if (*p == '"') p++;
    }
    return 0;
}

/* ── Module Registration ──────────────────────────────────────────── */

static int echo_execute(const char *args_json, char *result,
                        size_t result_len, void *user_data)
{
    (void)user_data;
    return os_snprintf(result, result_len,
                       "{\"echo\":%s}", args_json ? args_json : "{}");
}

static int list_dir_execute(const char *args_json, char *result,
                            size_t result_len, void *user_data)
{
    (void)user_data;
    if (result_len < 4) return -1;
    result[0] = '\0';
    size_t pos = 0;

    /* Normalize JSON: replace single quotes with double quotes */
    char norm_args[1024];
    if (args_json) {
        size_t alen = os_strlen(args_json);
        if (alen >= sizeof(norm_args)) alen = sizeof(norm_args) - 1;
        for (size_t i = 0; i < alen; i++)
            norm_args[i] = (args_json[i] == '\'') ? '"' : args_json[i];
        norm_args[alen] = '\0';
        args_json = norm_args;
    }

    /* Parse directory path from JSON args: {"path":"/foo"} */
    const char *dir_path = ".";
    if (args_json) {
        const char *p = strstr(args_json, "\"path\":\"");
        if (p) {
            p += 8;
            char path_buf[256];
            size_t pi = 0;
            while (p[pi] && p[pi] != '"' && pi < sizeof(path_buf) - 1) {
                path_buf[pi] = p[pi];
                pi++;
            }
            path_buf[pi] = '\0';
            if (pi > 0) dir_path = path_buf;
        }
    }

    os_dir_handle_t dh = os_dir_open(dir_path);
    if (!dh) {
        return os_snprintf(result, result_len, "{\"error\":\"cannot open directory\"}");
    }

    pos += os_snprintf(result + pos, result_len - pos, "{\"entries\":[");
    int first = 1;
    const char *entry;
    while ((entry = os_dir_next(dh)) != NULL && pos < result_len - 50) {
        if (!first) { pos += os_snprintf(result + pos, result_len - pos, ","); }
        first = 0;
        pos += os_snprintf(result + pos, result_len - pos, "\"%s\"", entry);
    }
    os_dir_close(dh);

    pos += os_snprintf(result + pos, result_len - pos, "]}");
    return 0;
}

static int read_file_execute(const char *args_json, char *result,
                             size_t result_len, void *user_data)
{
    (void)user_data;
    if (result_len < 4) return -1;
    result[0] = '\0';

    /* Normalize JSON: replace single quotes with double quotes */
    char norm_args[1024];
    if (args_json) {
        size_t alen = os_strlen(args_json);
        if (alen >= sizeof(norm_args)) alen = sizeof(norm_args) - 1;
        for (size_t i = 0; i < alen; i++)
            norm_args[i] = (args_json[i] == '\'') ? '"' : args_json[i];
        norm_args[alen] = '\0';
        args_json = norm_args;
    }

    /* Parse path from JSON: {"path":"/foo/bar"} */
    const char *path_start = strstr(args_json ? args_json : "", "\"path\":\"");
    if (!path_start) {
        return os_snprintf(result, result_len, "{\"error\":\"missing path argument\"}");
    }
    path_start += 8;
    char path[512];
    size_t pi = 0;
    while (path_start[pi] && path_start[pi] != '"' && pi < sizeof(path) - 1) {
        path[pi] = path_start[pi];
        pi++;
    }
    path[pi] = '\0';

    os_file_handle_t fh = os_file_open(path, "r");
    if (!fh) {
        return os_snprintf(result, result_len, "{\"error\":\"cannot open file: %s\"}", path);
    }

    size_t n = os_file_read(fh, result + 1, result_len - 4);
    os_file_close(fh);
    result[0] = '{';
    size_t rp = 1 + n;
    result[rp] = '}';
    result[rp + 1] = '\0';
    return 0;
}

static int tool_manager_init(framework_module_t *mod)
{
    g_ctx = (tool_manager_ctx_t *)os_calloc(1, sizeof(tool_manager_ctx_t));
    if (!g_ctx) return -1;
    mod->ctx = g_ctx;

    g_ctx->tool_mutex = os_mutex_create();
    if (!g_ctx->tool_mutex) {
        LOG_ERROR("ToolManager: mutex create failed");
        return -1;
    }

    g_ctx->tool_count = 0;
    for (int i = 0; i < MAX_TOOLS; i++) {
        g_ctx->tools[i].active = 0;
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
        .risk = TOOL_RISK_SAFE,
        .execute = echo_execute,
    };
    tool_register(&echo);

    /* Register code tools */
    tool_def_t list_dir = {
        .name = "list_dir",
        .description = "List directory contents",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"directory path\"}"
            "}}",
        .risk = TOOL_RISK_SAFE,
        .execute = list_dir_execute,
    };
    tool_register(&list_dir);

    tool_def_t read_file = {
        .name = "read_file",
        .description = "Read file content",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"file path\"}"
            "}}",
        .risk = TOOL_RISK_SAFE,
        .execute = read_file_execute,
    };
    tool_register(&read_file);

    LOG_INFO("ToolManager: ready (%d tool%s)",
             g_ctx->tool_count, g_ctx->tool_count == 1 ? "" : "s");
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
    if (g_ctx->tool_mutex) {
        os_mutex_destroy(g_ctx->tool_mutex);
        g_ctx->tool_mutex = NULL;
    }
    LOG_INFO("ToolManager: deinit");
    return 0;
}

    framework_module_t tool_manager_mod = {
    .name     = "tool_manager",
    .version  = 0x00010000,
    
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
