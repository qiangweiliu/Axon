/*
 * agent_loop.c — Agent decision loop (REPL + framework-driven mode)
 *
 * Handles interactive REPL, command dispatch, history, banner,
 * module registration, and prompt.txt file-driven mode.
 *
 * Sub-modules:
 *   ask.c       — LLM interaction (spinner, streaming, stats)
 *   handlers.c  — command handlers (note, profile, replace, forget, etc.)
 *   memfile.c   — bounded memory file store
 *   input.c     — raw terminal input
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "agent_loop.h"
#include "agent_private.h"
#include "input.h"
#include "ask.h"
#include "handlers.h"
#include "config.h"
#include "archive.h"
#include <stdio.h>
#include <string.h>

agent_loop_ctx_t *g_ctx = NULL;
volatile int g_spinner_on = 0;

static int process_line(const char *line, char *out, size_t out_len)
{
    if (!line || !*line) return 0;

    if (os_strncmp(line, "note ", 5) == 0) {
        handle_note(line + 5, out, out_len);
    } else if (os_strncmp(line, "profile ", 8) == 0) {
        handle_profile(line + 8, out, out_len);
    } else if (os_strcmp(line, "notes") == 0) {
        handle_notes(out, out_len);
    } else if (os_strncmp(line, "replace ", 8) == 0) {
        handle_replace(line + 8, out, out_len);
    } else if (os_strncmp(line, "forget ", 7) == 0) {
        handle_forget(line + 7, out, out_len);
    } else if (os_strcmp(line, "forget") == 0) {
        handle_forget("", out, out_len);
    } else if (os_strncmp(line, "echo ", 5) == 0) {
        handle_echo(line + 5, out, out_len);
    } else if (os_strncmp(line, "remember ", 9) == 0) {
        handle_remember(line + 9, out, out_len);
    } else if (os_strncmp(line, "recall ", 7) == 0) {
        int n = handle_recall(line + 7, out, out_len);
        if (n == 0 && out)
            os_snprintf(out, out_len, GRY "nothing found" RST);
    } else if (os_strncmp(line, "ask ", 4) == 0) {
        return handle_ask(line + 4, out, out_len);
    } else if (os_strcmp(line, "help") == 0) {
        if (out) os_snprintf(out, out_len,
            BLU "  Commands:" RST "\n"
            "\n"
            "  " GRN "ask" RST " <question>\n"
            "    Send a question to the LLM. The response is streamed\n"
            "    from the configured model (deepseek-v4-flash).\n"
            "    Example: " CYN "ask 今天天气怎么样？" RST "\n"
            "\n"
            "  " CYN "note" RST " <text>\n" \
            "    Save a fact to persistent memory (L0 working.md).\n" \
            "    Memory is loaded at startup and injected into every\n" \
            "    LLM prompt as context. Limits: 2200 chars total.\n" \
            "    Example: note 用户喜欢简洁的回答\n" \
            "\n" \
            "  " CYN "profile" RST " <text>\n" \
            "    Save user profile information (L0 profile.md).\n" \
            "    Same mechanism as note, but for user-specific data.\n" \
            "    Limits: 1375 chars total.\n" \
            "    Example: profile Name is 老板鱼饭\n" \
            "\n" \
            "  notes\n" \
            "    List all entries in both L0 working.md and profile.md\n" \
            "    with current usage percentage.\n" \
            "    Example: notes\n" \
            "\n" \
            "  replace <key> <new text>\n" \
            "    Find the first memory entry containing <key> and\n" \
            "    replace its entire content with <new text>.\n" \
            "    Only operates on working.md (use forget+profile for\n" \
            "    profile.md edits).\n" \
            "    Example: replace 简洁 用户偏好详细的回答\n" \
            "\n" \
            "  forget [-m|-u] <substring>\n" \
            "    Remove entries containing <substring> from memory.\n" \
            "    With no flag, removes from both working.md and profile.md.\n" \
            "    Use -m to target working.md only, -u for profile.md only.\n" \
            "    Example: forget -m 测试\n" \
            "    Example: forget 测试          (removes from both)\n"
            "\n"
            "  " CYN "echo" RST " <message>\n"
            "    Call the built-in echo tool. Returns the message\n"
            "    wrapped in a JSON response. Useful for testing.\n"
            "    Example: " CYN "echo hello world" RST "\n"
            "\n"
            "  " CYN "remember" RST " <text>\n"
            "    Store a long-term structured memory entry in the\n"
            "    file-based DB (data/memory.db). Unlike note, this\n"
            "    is not bounded and supports search via recall.\n"
            "    Example: " CYN "remember User prefers dark mode" RST "\n"
            "\n"
            "  " CYN "recall" RST " <query>\n"
            "    Search long-term DB memory for entries containing\n"
            "    the query string. Returns up to 4 matches.\n"
            "    Example: " CYN "recall dark mode" RST "\n"
            "\n"
            "  " GRY "help" RST "\n"
            "    Show this detailed help with examples.\n"
            "\n"
            "  " GRY "exit" RST "  /  " GRY "quit" RST "\n" \
            "    Exit the REPL and shutdown.\n" \
            "    Example: " CYN "exit" RST "\n" \
            "\n" \
            "  " CYN "clear" RST "\n" \
            "    Clear ALL memories (L0-L5). Irreversible.\n" \
            "    Wipes topics, events, archive logs, and memory.db.\n" \
            "    Example: " CYN "clear" RST "");
    } else if (os_strcmp(line, "exit") == 0 ||
               os_strcmp(line, "quit") == 0) {
        /* Flush remaining conversation segment */
        archive_flush_segment(ARC_IMP_MEDIUM);
        if (out) os_snprintf(out, out_len, "bye");
        return 1;
    } else if (os_strcmp(line, "clear") == 0 ||
               os_strcmp(line, "memory clear") == 0) {
        archive_clear_all();
        if (out) os_snprintf(out, out_len, GRN "✓ All memory cleared" RST);
    } else {
        return handle_ask(line, out, out_len);
    }
    return 0;
}

/* ── Command History ──────────────────────────────────────────────── */

#define HIST_MAX 50

static char  g_hist[HIST_MAX][PROMPT_MAX];
static int   g_hist_count;   /* total entered */
static int   g_hist_pos;     /* -1 = new input, 0..count-1 = browsing */

static void hist_push(const char *line)
{
    if (!line || !*line) return;
    if (g_hist_count > 0 &&
        os_strcmp(g_hist[(g_hist_count-1) % HIST_MAX], line) == 0)
        return;
    size_t len = os_strlen(line);
    if (len >= PROMPT_MAX) len = PROMPT_MAX - 1;
    os_memcpy(g_hist[g_hist_count % HIST_MAX], line, len);
    g_hist[g_hist_count % HIST_MAX][len] = '\0';
    g_hist_count++;
    g_hist_pos = -1;
}

/* ── REPL (Interactive) ───────────────────────────────────────────── */

void agent_loop_repl(void)
{
    const config_t *cfg = config_get();
    const char *model = cfg && cfg->llm_model[0]
                        ? cfg->llm_model : "gpt-4";
    const char *ep    = cfg && cfg->llm_endpoint[0]
                        ? cfg->llm_endpoint : "(unset)";

    /* Load persistent memory files */
    memfile_load("data/memory/l0/working.md", &g_ctx->mem, MEMFILE_MEMORY_LIMIT);
    memfile_load("data/memory/l0/profile.md",   &g_ctx->user, MEMFILE_USER_LIMIT);
    g_ctx->session_tokens = 0;

    char mu[64], uu[64];
    memfile_usage(&g_ctx->mem, mu, sizeof(mu));
    memfile_usage(&g_ctx->user, uu, sizeof(uu));

    /* ── Banner ── */
    os_printf(BLD BLU "┌" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++)
        os_printf(BLD BLU "─" RST);
    os_printf(BLD BLU "┐\n" RST);
    os_printf(BLD BLU "│" RST "  Axon  " DIM "·" RST "  %s\n", model);
    os_printf(BLD BLU "│" RST "  " GRY "%s" RST "\n", ep);
    os_printf(BLD BLU "├" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++) os_printf("─");
    os_printf(BLD BLU "┤\n" RST);
    os_printf(BLD BLU "│" RST "  " CYN "mem"
              RST " " GRY "%s" RST "  " CYN "you"
              RST " " GRY "%s" RST "\n", mu, uu);
    os_printf(BLD BLU "│" RST "  " GRN "ask" RST "  " DIM "·" RST
              "  " CYN "note" RST "  " DIM "·" RST
              "  " CYN "notes" RST "  " DIM "·" RST
              "  " CYN "forget" RST "  " DIM "·" RST
              "  " GRY "help" RST "\n");
    os_printf(BLD BLU "└" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++)
        os_printf(BLD BLU "─" RST);
    os_printf(BLD BLU "┘\n\n" RST);

    char line[PROMPT_MAX];
    raw_on();
    for (;;) {
        os_printf(BLD GRN "┃> " RST);
        fflush(stdout);

        int n = read_line_raw(line, PROMPT_MAX);
        if (n < 0) { os_printf("\n" GRY "EOF" RST "\n"); break; }

        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) continue;

        hist_push(t);

        char result[4096] = "";
        int rc = process_line(t, result, sizeof(result));

        if (rc != 0 && strcmp(result, "bye") == 0) {
            os_printf(GRY "┃ Goodbye" RST "\n");
            break;
        }
        if (result[0]) {
            os_printf("\n%s\n", result);
        }
    }
    raw_off();
}

/* ── Framework-Driven Mode (prompt.txt) ───────────────────────────── */

static int agent_loop_init(framework_module_t *mod)
{
    g_ctx = (agent_loop_ctx_t *)os_calloc(1, sizeof(agent_loop_ctx_t));
    if (!g_ctx) return -1;
    mod->ctx = g_ctx;
    g_ctx->tick_count = 0;

    /* Load persistent memory files */
    memfile_load("data/memory/l0/working.md", &g_ctx->mem, MEMFILE_MEMORY_LIMIT);
    memfile_load("data/memory/l0/profile.md",   &g_ctx->user, MEMFILE_USER_LIMIT);
    g_ctx->session_tokens = 0;

    os_file_handle_t fh = os_file_open(g_ctx->prompt_path, "a");
    if (fh) os_file_close(fh);
    const config_t *cfg = config_get();
    LOG_INFO("Agent: init (llm=%s, model=%s)",
             cfg && cfg->llm_endpoint[0] ? cfg->llm_endpoint : "(unset)",
             cfg && cfg->llm_model[0] ? cfg->llm_model : "gpt-4");
    return 0;
}

static int agent_loop_start(framework_module_t *mod)
{
    (void)mod; LOG_INFO("Agent: ready"); return 0;
}

static void agent_loop_tick(framework_module_t *mod)
{
    (void)mod;
    g_ctx->tick_count++;
    if (g_ctx->tick_count < TICK_COOLDOWN) return;

    os_file_handle_t fh = os_file_open(g_ctx->prompt_path, "r");
    if (!fh) return;
    char line[PROMPT_MAX];
    size_t pos = 0;
    int processed = 0;
    while (processed < 3) {
        size_t nr = os_file_read(fh, &line[pos], 1);
        if (nr == 0) break;
        if (line[pos] == '\n' || pos >= PROMPT_MAX - 2) {
            line[pos] = '\0'; pos = 0;
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (*t && *t != '#') {
                char result[4096] = "";
                process_line(t, result, sizeof(result));
                if (result[0]) LOG_INFO("Agent: %s", result);
                processed++;
            }
        } else { pos++; }
    }
    os_file_close(fh);
    if (processed > 0) {
        os_file_handle_t out = os_file_open(g_ctx->prompt_path, "w");
        if (out) os_file_close(out);
    }
    g_ctx->tick_count = 0;
}

void agent_set_prompt_file(const char *path)
{
    if (path) {
        size_t len = os_strlen(path);
        if (len < sizeof(g_ctx->prompt_path))
            { os_memcpy(g_ctx->prompt_path, path, len + 1); }
    }
}

    framework_module_t agent_loop_mod = {
    .name = "agent_loop", .version = 0x00040000, 
    .state = FRAMEWORK_STATE_UNLOADED, .init = agent_loop_init,
    .start = agent_loop_start, .loop = agent_loop_tick,
    .stop = NULL, .deinit = NULL, .ctx = NULL, .id = 0, .next = NULL,
};
MODULE_REGISTER(agent_loop_mod);
