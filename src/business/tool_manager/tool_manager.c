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
    LOG_DEBUG("ToolManager:   risk=%d, schema_len=%zu",
              tool->risk, tool->params_json ? os_strlen(tool->params_json) : 0);
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
            LOG_DEBUG("ToolManager:   args (first 200): %.*s",
                      (int)(os_strlen(args_json) < 200 ? os_strlen(args_json) : 200),
                      args_json);
            os_mutex_unlock(g_ctx->tool_mutex);

            int rc = t->execute(args_json, result, result_len, NULL);
            LOG_DEBUG("ToolManager:   result: rc=%d, output_len=%zu",
                      rc, result ? os_strlen(result) : (size_t)0);
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
    LOG_DEBUG("ToolManager: list_json: %d tool%s serialized, buf=%zu/%zu",
              count, count == 1 ? "" : "s", pos, buf_len);
    return count;
}

int tool_count(void)
{
    os_mutex_lock(g_ctx->tool_mutex);
    int n = g_ctx->tool_count;
    os_mutex_unlock(g_ctx->tool_mutex);
    LOG_DEBUG("ToolManager: count=%d", n);
    return n;
}

int tool_get_info(int index, tool_info_t *info)
{
    if (!info) return -1;
    os_mutex_lock(g_ctx->tool_mutex);
    if (index < 0 || index >= g_ctx->tool_count) {
        os_mutex_unlock(g_ctx->tool_mutex);
        LOG_DEBUG("ToolManager: get_info(%d) out of range (count=%d)", index, g_ctx->tool_count);
        return -1;
    }
    tool_entry_t *e = &g_ctx->tools[index];
    info->name = e->def.name;
    info->description = e->def.description;
    info->params_json = e->def.params_json;
    info->risk = e->def.risk;
    info->enabled = e->active;
    os_mutex_unlock(g_ctx->tool_mutex);
    LOG_DEBUG("ToolManager: get_info(%d) -> '%s'", index, info->name);
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
            LOG_DEBUG("ToolManager: find('%s') found (risk=%d)", name, info->risk);
            return 0;
        }
    }
    os_mutex_unlock(g_ctx->tool_mutex);
    LOG_DEBUG("ToolManager: find('%s') not found", name);
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
        LOG_DEBUG("ToolManager: validate('%s') failed: unknown tool", name);
        return -1;
    }
    if (!info.params_json || !*info.params_json) {
        LOG_DEBUG("ToolManager: validate('%s') — no schema, skipping", name);
        return 0;  /* no schema = no validation */
    }

    /* Lightweight: check required fields exist in args_json */
    /* Parse schema for "required": [...] and check each key exists in args */
    const char *req = strstr(info.params_json, "\"required\"");
    if (!req) {
        LOG_DEBUG("ToolManager: validate('%s') — no required fields in schema", name);
        return 0;  /* no required fields */
    }

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
                LOG_DEBUG("ToolManager: validate('%s') failed: missing '%.*s'",
                          name, (int)fn_len, fn_start);
                return -1;
            }
        }
        /* Skip past quote */
        if (*p == '"') p++;
    }
    LOG_DEBUG("ToolManager: validate('%s') passed", name);
    return 0;
}

/* ── Module Registration ──────────────────────────────────────────── */

static int echo_execute(const char *args_json, char *result,
                        size_t result_len, void *user_data)
{
    (void)user_data;
    int n = os_snprintf(result, result_len,
                        "{\"echo\":%s}", args_json ? args_json : "{}");
    LOG_DEBUG("ToolManager: echo -> %d bytes", n);
    return n;
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
        LOG_DEBUG("ToolManager: list_dir cannot open '%s'", dir_path);
        return os_snprintf(result, result_len, "{\"error\":\"cannot open directory\"}");
    }

    pos += os_snprintf(result + pos, result_len - pos, "{\"entries\":[");
    int first = 1, entry_count = 0;
    const char *entry;
    while ((entry = os_dir_next(dh)) != NULL && pos < result_len - 50) {
        if (!first) { pos += os_snprintf(result + pos, result_len - pos, ","); }
        first = 0;
        pos += os_snprintf(result + pos, result_len - pos, "\"%s\"", entry);
        entry_count++;
    }
    os_dir_close(dh);
    LOG_DEBUG("ToolManager: list_dir '%s' -> %d entries", dir_path, entry_count);

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
        LOG_DEBUG("ToolManager: read_file cannot open '%s'", path);
        return os_snprintf(result, result_len, "{\"error\":\"cannot open file: %s\"}", path);
    }

    size_t n = os_file_read(fh, result + 1, result_len - 4);
    os_file_close(fh);
    result[0] = '{';
    size_t rp = 1 + n;
    result[rp] = '}';
    result[rp + 1] = '\0';
    LOG_DEBUG("ToolManager: read_file '%s' -> %zu bytes", path, n);
    return 0;
}

/* ── write_file ──────────────────────────────────────────────────── */

static int write_file_execute(const char *args_json, char *result,
                               size_t result_len, void *user_data)
{
    (void)user_data;
    if (result_len < 4) return -1;
    result[0] = '\0';

    /* Parse path and content from JSON */
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

    const char *content_start = strstr(args_json, "\"content\":\"");
    if (!content_start) {
        return os_snprintf(result, result_len, "{\"error\":\"missing content argument\"}");
    }
    content_start += 11;

    /* Extract content (handle escaped chars: \n, \t, \\, \") */
    char content_buf[16384];
    size_t ci = 0;
    while (content_start[ci] && ci < sizeof(content_buf) - 2) {
        if (content_start[ci] == '\\' && content_start[ci+1] == '"') {
            content_buf[ci] = '"';
            ci++; content_start++;
            continue;
        }
        if (content_start[ci] == '\\' && content_start[ci+1] == 'n') {
            content_buf[ci] = '\n';
            ci++; content_start++;
            continue;
        }
        if (content_start[ci] == '\\' && content_start[ci+1] == 't') {
            content_buf[ci] = '\t';
            ci++; content_start++;
            continue;
        }
        if (content_start[ci] == '\\' && content_start[ci+1] == '\\') {
            content_buf[ci] = '\\';
            ci++; content_start++;
            continue;
        }
        if (content_start[ci] == '"') break;
        content_buf[ci] = content_start[ci];
        ci++;
    }
    content_buf[ci] = '\0';

    os_file_handle_t fh = os_file_open(path, "w");
    if (!fh) {
        LOG_DEBUG("ToolManager: write_file cannot open '%s'", path);
        return os_snprintf(result, result_len, "{\"error\":\"cannot open file: %s\"}", path);
    }

    size_t written = os_file_write(fh, content_buf, os_strlen(content_buf));
    os_file_close(fh);

    LOG_DEBUG("ToolManager: write_file '%s' -> %zu bytes", path, written);
    return os_snprintf(result, result_len,
                       "{\"status\":\"ok\",\"path\":\"%s\",\"bytes\":%zu}",
                       path, written);
}

/* ── bash ─────────────────────────────────────────────────────────── */

static int bash_execute(const char *args_json, char *result,
                         size_t result_len, void *user_data)
{
    (void)user_data;
    if (result_len < 4) return -1;
    result[0] = '\0';

    const char *cmd_start = strstr(args_json ? args_json : "", "\"command\":\"");
    if (!cmd_start) {
        return os_snprintf(result, result_len, "{\"error\":\"missing command argument\"}");
    }
    cmd_start += 11;  /* skip past "command":" (11 chars) */

    char cmd_buf[4096];
    size_t ci = 0;
    while (cmd_start[ci] && ci < sizeof(cmd_buf) - 2) {
        if (cmd_start[ci] == '\\' && cmd_start[ci+1] == '"') {
            cmd_buf[ci] = '"';
            ci++; cmd_start++;
            continue;
        }
        if (cmd_start[ci] == '\\' && cmd_start[ci+1] == 'n') {
            cmd_buf[ci] = ' ';
            ci++; cmd_start++;
            continue;
        }
        if (cmd_start[ci] == '\\' && cmd_start[ci+1] == '\\') {
            cmd_buf[ci] = '\\';
            ci++; cmd_start++;
            continue;
        }
        if (cmd_start[ci] == '"') break;
        cmd_buf[ci] = cmd_start[ci];
        ci++;
    }
    cmd_buf[ci] = '\0';

    LOG_DEBUG("ToolManager: bash cmd (first 300): %.*s",
              (int)(os_strlen(cmd_buf) < 300 ? os_strlen(cmd_buf) : 300), cmd_buf);

    /* Safety check: block destructive commands */
    {
        const char *danger[] = {
            "rm -rf /", "rm -fr /", "rm -rf ~", "mkfs.", "dd if=",
            ":(){ :|:& };:", "> /dev/sda", "chmod 777 /",
            "wget", "curl",  /* network download — allow if whitelisted */
        };
        int blocked = 0;
        for (size_t di = 0; di < sizeof(danger)/sizeof(danger[0]); di++) {
            if (strstr(cmd_buf, danger[di])) {
                /* Allow wget/curl to local paths only */
                if ((os_strncmp(danger[di], "wget", 4) == 0 ||
                     os_strncmp(danger[di], "curl", 4) == 0) &&
                    !strstr(cmd_buf, " /tmp/") && !strstr(cmd_buf, " /home/"))
                    continue;
                blocked = 1;
                break;
            }
        }
        if (blocked) {
            return os_snprintf(result, result_len,
                "{\"error\":\"blocked: unsafe command pattern detected\"}");
        }
    }

    /* Execute via popen with 30s timeout */
    char timeout_cmd[4096 + 32];
    os_snprintf(timeout_cmd, sizeof(timeout_cmd),
                "timeout 30 %s", cmd_buf);
    FILE *fp = popen(timeout_cmd, "r");
    if (!fp) {
        LOG_DEBUG("ToolManager: bash popen failed for '%s'", cmd_buf);
        return os_snprintf(result, result_len,
                           "{\"error\":\"popen failed\",\"command\":\"%s\"}", cmd_buf);
    }

    size_t pos = 0;
    pos += os_snprintf(result + pos, result_len - pos, "{\"output\":\"");
    char line[1024];
    while (fgets(line, sizeof(line), fp) && pos < result_len - 200) {
        /* Escape special chars for JSON */
        for (char *lp = line; *lp && pos < result_len - 200; lp++) {
            if (*lp == '"') {
                if (pos < result_len - 4) { result[pos++] = '\\'; result[pos++] = '"'; }
            } else if (*lp == '\\') {
                if (pos < result_len - 4) { result[pos++] = '\\'; result[pos++] = '\\'; }
            } else if (*lp == '\n') {
                if (pos < result_len - 4) { result[pos++] = '\\'; result[pos++] = 'n'; }
            } else if (*lp == '\t') {
                if (pos < result_len - 4) { result[pos++] = '\\'; result[pos++] = 't'; }
            } else {
                if (pos < result_len - 3) result[pos++] = *lp;
            }
        }
    }
    int exit_code = pclose(fp);
    LOG_DEBUG("ToolManager: bash exit_code=%d, output_len=%zu", exit_code, pos);
    if (pos < result_len - 50) {
        os_snprintf(result + pos, result_len - pos,
                    "\",\"exit_code\":%d}", exit_code);
    }
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

    /* Write file tool */
    tool_def_t write_file = {
        .name = "write_file",
        .description = "Write content to a file. Creates the file if it doesn't exist, overwrites if it does.",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"file path to write\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"content to write\"}"
            "},\"required\":[\"path\",\"content\"]}",
        .risk = TOOL_RISK_WRITE,
        .execute = write_file_execute,
    };
    tool_register(&write_file);

    /* Bash shell tool */
    tool_def_t bash = {
        .name = "bash",
        .description = "Execute a shell command and return its stdout/stderr output",
        .params_json = "{\"type\":\"object\",\"properties\":{"
            "\"command\":{\"type\":\"string\",\"description\":\"shell command to run\"}"
            "},\"required\":[\"command\"]}",
        .risk = TOOL_RISK_SHELL,
        .execute = bash_execute,
    };
    tool_register(&bash);

    /* ── Load dynamic tools from data/tools/*.json ────────────────── */
    {
        os_dir_handle_t dh = os_dir_open("data/tools");
        if (dh) {
            const char *entry;
            while ((entry = os_dir_next(dh)) != NULL) {
                size_t elen = os_strlen(entry);
                if (elen < 6) continue;  /* at least "x.json" */
                const char *ext = entry + elen - 5;
                if (os_strcmp(ext, ".json") != 0) continue;

                char path[512];
                os_snprintf(path, sizeof(path), "data/tools/%s", entry);
                os_file_handle_t fh = os_file_open(path, "r");
                if (!fh) continue;
                char jbuf[4096];
                size_t nr = os_file_read(fh, jbuf, sizeof(jbuf) - 1);
                os_file_close(fh);
                if (nr == 0) continue;
                jbuf[nr] = '\0';

                /* Simple JSON extract: "name", "description", "risk" */
                char dname[64] = "";
                char ddesc[256] = "";
                int  drisk = TOOL_RISK_SAFE;

                const char *nk = strstr(jbuf, "\"name\"");
                if (nk) {
                    nk = strchr(nk, ':');
                    if (nk) {
                        nk++; while (*nk == ' ' || *nk == '"') nk++;
                        size_t i = 0;
                        while (nk[i] && nk[i] != '"' && i < sizeof(dname)-1)
                            { dname[i] = nk[i]; i++; }
                        dname[i] = '\0';
                    }
                }
                const char *dk = strstr(jbuf, "\"description\"");
                if (dk) {
                    dk = strchr(dk, ':');
                    if (dk) {
                        dk++; while (*dk == ' ' || *dk == '"') dk++;
                        size_t i = 0;
                        while (dk[i] && dk[i] != '"' && i < sizeof(ddesc)-1)
                            { ddesc[i] = dk[i]; i++; }
                        ddesc[i] = '\0';
                    }
                }
                const char *rk = strstr(jbuf, "\"risk\"");
                if (rk) {
                    rk = strchr(rk, ':');
                    if (rk) {
                        rk++; while (*rk == ' ' || *rk == '"') rk++;
                        if (os_strncmp(rk, "shell", 5) == 0) drisk = TOOL_RISK_SHELL;
                        else if (os_strncmp(rk, "write", 5) == 0) drisk = TOOL_RISK_WRITE;
                    }
                }

                if (!dname[0]) continue;
                /* Skip if already registered (built-in) */
                tool_info_t exist;
                if (tool_find(dname, &exist) == 0) continue;

                tool_def_t dt;
                os_memset(&dt, 0, sizeof(dt));
                dt.name = dname;
                dt.description = ddesc;
                dt.params_json = "{\"type\":\"object\",\"properties\":{}}";
                dt.risk = drisk;
                dt.execute = echo_execute;  /* safe generic handler */
                tool_register(&dt);
            }
            os_dir_close(dh);
        }
    }

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
