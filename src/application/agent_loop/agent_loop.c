/*
 * agent_loop.c — Agent decision loop
 *
 * Two modes:
 *   framework-driven: loop() reads prompt.txt, processes, logs results.
 *   interactive:      repl() reads stdin, displays responses to stdout.
 *
 * Terminal UI inspired by Hermes Agent CLI.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "tool_manager.h"
#include "memory.h"
#include "llm_client.h"
#include "config.h"
#include "agent_loop.h"
#include <stdio.h>
#include <string.h>

#define PROMPT_PATH_DEFAULT "prompt.txt"
#define PROMPT_MAX          1024
#define TICK_COOLDOWN       5

/* ── ANSI Terminal Colors ─────────────────────────────────────────── */

#define RST  "\033[0m"
#define BLD  "\033[1m"
#define DIM  "\033[2m"
#define GRN  "\033[32m"
#define CYN  "\033[36m"
#define YLW  "\033[33m"
#define GRY  "\033[90m"
#define BLU  "\033[34m"
#define RED  "\033[31m"

/* ── Globals ──────────────────────────────────────────────────────── */

static char g_prompt_path[256] = PROMPT_PATH_DEFAULT;
static int  g_tick_count;

/* ── Terminal Helpers ─────────────────────────────────────────────── */

#define LINE_WIDTH 60

static void print_sep(char c)
{
    for (int i = 0; i < LINE_WIDTH; i++) os_printf("%c", c);
    os_printf("\n");
}

/* ── Spinner ──────────────────────────────────────────────────────── */

static volatile int g_spinner_on = 0;

static void *spinner_thread(void *arg)
{
    (void)arg;
    const char *frames[] = { "◇", "◆", "◇", "◆" };
    int i = 0;
    while (g_spinner_on) {
        os_printf("\r  " CYN "%s" RST " Thinking...", frames[i]);
        fflush(stdout);
        i = (i + 1) % 4;
        os_sleep_ms(250);
    }
    os_printf("\r");
    fflush(stdout);
    return NULL;
}

/* ── Handlers ─────────────────────────────────────────────────────── */

static void handle_echo(const char *msg, char *out, size_t out_len)
{
    char args[512];
    os_snprintf(args, sizeof(args), "{\"msg\":\"%s\"}", msg ? msg : "");
    char result[TOOL_RESULT_MAX];
    if (tool_call("echo", args, result, sizeof(result)) >= 0) {
        if (out) os_snprintf(out, out_len, YLW "%s" RST, result);
    }
}

static void handle_remember(const char *content, char *out, size_t out_len)
{
    if (!content || !*content) return;
    memory_entry_t e;
    os_memset(&e, 0, sizeof(e));
    os_memcpy(e.type, "note", 5);
    size_t clen = os_strlen(content);
    if (clen >= MEMORY_CONTENT_MAX) clen = MEMORY_CONTENT_MAX - 1;
    os_memcpy(e.content, content, clen);
    char id[MEMORY_ID_MAX];
    if (memory_store(&e, id, sizeof(id)) == 0) {
        if (out) os_snprintf(out, out_len, GRY "✓ stored" RST);
    }
}

static int handle_recall(const char *query, char *out, size_t out_len)
{
    memory_entry_t results[4];
    int found;
    if (memory_search(query, results, 4, &found) == 0 && found > 0) {
        if (out) {
            size_t pos = 0;
            pos += os_snprintf(out + pos, out_len - pos,
                               GRY "%d result(s)" RST, found);
            for (int i = 0; i < found && pos < out_len; i++) {
                pos += os_snprintf(out + pos, out_len - pos,
                                   "\n  " GRY "·" RST " %s",
                                   results[i].content);
            }
        }
        return found;
    }
    return 0;
}

/* Token callback — first token stops spinner, then prints on a new line */
static int g_first_token = 0;

static void on_llm_token(const char *token, size_t len,
                         int tokens_so_far, uint64_t elapsed_ms,
                         void *user)
{
    (void)len; (void)tokens_so_far; (void)elapsed_ms; (void)user;
    if (!g_first_token) {
        g_first_token = 1;
        g_spinner_on = 0;               /* stop spinner */
        os_sleep_ms(300);                /* wait for spinner to finish its 250ms cycle */
        os_printf("\n");                /* new line after prompt */
    }
    os_printf("%s", token);
    fflush(stdout);
}

static int handle_ask(const char *question, char *out, size_t out_len)
{
    if (!question || !*question) return -1;

    const config_t *cfg = config_get();
    const char *endpoint = cfg && cfg->llm_endpoint[0]
                           ? cfg->llm_endpoint : "http://localhost:8080/v1";
    const char *api_key  = cfg && cfg->llm_api_key[0]
                           ? cfg->llm_api_key : NULL;
    const char *model    = cfg && cfg->llm_model[0]
                           ? cfg->llm_model : "gpt-4";

    g_spinner_on = 1;
    g_first_token = 0;
    os_thread_handle_t tid;
    os_thread_create(&tid, spinner_thread, NULL);

    uint64_t t0 = os_clock_ms();
    llm_response_t *resp = llm_chat_stream(endpoint, api_key, model,
                                           question, on_llm_token, NULL);
    uint64_t elapsed = os_clock_ms() - t0;

    g_spinner_on = 0;
    os_thread_join(tid);

    if (!resp) {
        if (out) os_snprintf(out, out_len, RED "%% LLM unavailable" RST);
        return -1;
    }

    resp->latency_ms = elapsed;

    /* Stats bar (dim, at bottom after the response) */
    if (out) {
        size_t pos = 0;
        pos += os_snprintf(out + pos, out_len - pos,
                           DIM "  %.1fs", (double)elapsed / 1000.0);
        if (resp->completion_tokens > 0) {
            pos += os_snprintf(out + pos, out_len - pos,
                               " · %d tok", resp->completion_tokens);
            if (elapsed > 0) {
                double tps = (double)resp->completion_tokens /
                             ((double)elapsed / 1000.0);
                pos += os_snprintf(out + pos, out_len - pos,
                                   " · %.1f tok/s", tps);
            }
        }
        os_snprintf(out + pos, out_len - pos, RST);
    }

    llm_response_free(resp);
    return 0;
}

/* ── Command Parser ───────────────────────────────────────────────── */

static int process_line(const char *line, char *out, size_t out_len)
{
    if (!line || !*line) return 0;

    if (os_strncmp(line, "echo ", 5) == 0) {
        handle_echo(line + 5, out, out_len);
    } else if (os_strncmp(line, "remember ", 9) == 0) {
        handle_remember(line + 9, out, out_len);
    } else if (os_strncmp(line, "recall ", 7) == 0) {
        int n = handle_recall(line + 7, out, out_len);
        if (n == 0 && out) os_snprintf(out, out_len, GRY "nothing found" RST);
    } else if (os_strncmp(line, "ask ", 4) == 0) {
        return handle_ask(line + 4, out, out_len);
    } else if (os_strcmp(line, "help") == 0) {
        if (out) os_snprintf(out, out_len,
            BLU "  Commands:" RST "\n"
            "  " GRN "ask" RST " <question>   — send to LLM\n"
            "  " CYN "echo" RST " <msg>       — call echo tool\n"
            "  " CYN "remember" RST " <text>  — store in memory\n"
            "  " CYN "recall" RST " <query>   — search memory\n"
            "  " GRY "help" RST "             — this help\n"
            "  " GRY "exit" RST "             — quit");
    } else if (os_strcmp(line, "exit") == 0 || os_strcmp(line, "quit") == 0) {
        if (out) os_snprintf(out, out_len, "bye");
        return 1;
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
    /* Don't store duplicate of last entry */
    if (g_hist_count > 0 && os_strcmp(g_hist[(g_hist_count-1) % HIST_MAX], line) == 0)
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
    const char *model = cfg && cfg->llm_model[0] ? cfg->llm_model : "gpt-4";
    const char *ep    = cfg && cfg->llm_endpoint[0] ? cfg->llm_endpoint : "(unset)";

    /* ── Banner ── */
    os_printf(BLD BLU "┌" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++) os_printf(BLD BLU "─" RST);
    os_printf(BLD BLU "┐\n" RST);
    os_printf(BLD BLU "│" RST "  Axon  " DIM "·" RST "  %s\n", model);
    os_printf(BLD BLU "│" RST "  " GRY "%s" RST "\n", ep);
    os_printf(BLD BLU "├" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++) os_printf("─");
    os_printf(BLD BLU "┤\n" RST);
    os_printf(BLD BLU "│" RST "  " GRN "ask" RST "  " DIM "·" RST "  " CYN "echo" RST
              "  " DIM "·" RST "  " CYN "remember" RST "  " DIM "·" RST
              "  " CYN "recall" RST "  " DIM "·" RST "  " GRY "help" RST "\n");
    os_printf(BLD BLU "└" RST);
    for (int i = 0; i < LINE_WIDTH - 2; i++) os_printf(BLD BLU "─" RST);
    os_printf(BLD BLU "┘\n\n" RST);

    char line[PROMPT_MAX];
    for (;;) {
        /* ── Prompt ── */
        os_printf(BLD GRN "┃> " RST);
        fflush(stdout);

        /* ── Read line ── */
        if (!fgets(line, sizeof(line), stdin)) {
            os_printf("\n" GRY "EOF" RST "\n");
            break;
        }

        /* Strip trailing \n */
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
            line[--llen] = '\0';

        /* Trim leading whitespace */
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) continue;

        /* ── Process ── */
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
}

/* ── Framework-Driven Mode (prompt.txt) ───────────────────────────── */

static int agent_loop_init(framework_module_t *mod)
{
    (void)mod;
    g_tick_count = 0;
    os_file_handle_t fh = os_file_open(g_prompt_path, "a");
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
    g_tick_count++;
    if (g_tick_count < TICK_COOLDOWN) return;

    os_file_handle_t fh = os_file_open(g_prompt_path, "r");
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
        os_file_handle_t out = os_file_open(g_prompt_path, "w");
        if (out) os_file_close(out);
    }
    g_tick_count = 0;
}

void agent_set_prompt_file(const char *path)
{
    if (path) {
        size_t len = os_strlen(path);
        if (len < sizeof(g_prompt_path))
            { os_memcpy(g_prompt_path, path, len + 1); }
    }
}

framework_module_t agent_loop_mod = {
    .name = "agent_loop", .version = 0x00030000, .priority = 10,
    .state = FRAMEWORK_STATE_UNLOADED, .init = agent_loop_init,
    .start = agent_loop_start, .loop = agent_loop_tick,
    .stop = NULL, .deinit = NULL, .ctx = NULL, .id = 0, .next = NULL,
};
MODULE_REGISTER(agent_loop_mod);
