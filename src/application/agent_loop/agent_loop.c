/*
 * agent_loop.c — Agent decision loop
 *
 * Two modes:
 *   framework-driven: loop() reads prompt.txt, processes, logs results.
 *   interactive:      repl() reads stdin, displays responses to stdout.
 *
 * Memory system (Hermes-style):
 *   MEMORY.md — agent's persistent notes (2200 chars limit)
 *   USER.md   — user profile (1375 chars limit)
 *   Entries separated by "\n§\n", injected into system prompt on ask.
 *   Commands: note, profile, notes, replace, forget
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
#include "memfile.h"
#include "input.h"
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

/* ── Persistent memory files (Hermes-style) ───────────────────────── */

#define MEMFILE_MEMORY_LIMIT  8000
#define MEMFILE_USER_LIMIT    4000

typedef struct {
    memfile_t mem;
    memfile_t user;
    char prompt_path[256];
    int  tick_count;
    int  session_tokens;
    int  first_token;
    int  saw_reasoning;   /* track transition reasoning→content */
} agent_loop_ctx_t;

static agent_loop_ctx_t *g_ctx = NULL;

/* ── Terminal Helpers ─────────────────────────────────────────────── */

#define LINE_WIDTH 60

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

/* ── Hermes-style bounded memory commands ─────────────────────────── */

static void handle_note(const char *text, char *out, size_t out_len)
{
    if (!text || !*text) return;
    if (memfile_add(&g_ctx->mem, text) != 0) {
        if (out) os_snprintf(out, out_len,
            RED "%% memory full" RST " — use " GRY "forget" RST " to free space");
        return;
    }
    memfile_save(&g_ctx->mem);
    char usage[64];
    memfile_usage(&g_ctx->mem, usage, sizeof(usage));
    if (out) os_snprintf(out, out_len, GRY "✓ noted  (%s)" RST, usage);
}

static void handle_profile(const char *text, char *out, size_t out_len)
{
    if (!text || !*text) return;
    if (memfile_add(&g_ctx->user, text) != 0) {
        if (out) os_snprintf(out, out_len,
            RED "%% profile full" RST " — use " GRY "forget" RST " to free space");
        return;
    }
    memfile_save(&g_ctx->user);
    char usage[64];
    memfile_usage(&g_ctx->user, usage, sizeof(usage));
    if (out) os_snprintf(out, out_len, GRY "✓ profile saved  (%s)" RST, usage);
}

/*
 * replace <key> <text>
 * Find the memory entry containing <key> and replace its content.
 * Targets memory (not profile). Use "forget -u" + "profile" for user edits.
 */
static void handle_replace(const char *args, char *out, size_t out_len)
{
    if (!args || !*args) return;

    /* Split on first space: key = before, text = after */
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (*p != ' ') {
        if (out) os_snprintf(out, out_len,
            GRY "usage: replace <key> <new text>" RST);
        return;
    }

    size_t key_len = (size_t)(p - args);
    const char *text = p;
    while (*text == ' ') text++;
    if (!*text) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: replace <key> <new text>" RST);
        return;
    }

    char key_buf[256];
    size_t kc = key_len < sizeof(key_buf) - 1
                ? key_len : sizeof(key_buf) - 1;
    os_memcpy(key_buf, args, kc);
    key_buf[kc] = '\0';

    if (memfile_replace(&g_ctx->mem, key_buf, text) == 0) {
        memfile_save(&g_ctx->mem);
        char usage[64];
        memfile_usage(&g_ctx->mem, usage, sizeof(usage));
        if (out) os_snprintf(out, out_len,
            GRY "✓ replaced  (%s)" RST, usage);
    } else {
        if (out) os_snprintf(out, out_len,
            RED "%% replace failed" RST " — key not found or would exceed limit");
    }
}

/*
 * forget [-m|-u] <substring>
 *   -m  : remove from memory only (default if omitted, i.e. both)
 *   -u  : remove from user profile only
 * Default (no flag): remove from both
 */
static void handle_forget(const char *raw, char *out, size_t out_len)
{
    if (!raw || !*raw) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: forget [-m|-u] <substring>" RST);
        return;
    }

    int do_mem  = 0;
    int do_user = 0;
    const char *sub = raw;

    /* Parse optional flag */
    if (*raw == '-') {
        if (raw[1] == 'm' && (raw[2] == ' ' || raw[2] == '\0')) {
            do_mem = 1;
            sub = raw[2] ? raw + 3 : "";
        } else if (raw[1] == 'u' && (raw[2] == ' ' || raw[2] == '\0')) {
            do_user = 1;
            sub = raw[2] ? raw + 3 : "";
        } else {
            if (out) os_snprintf(out, out_len,
                GRY "usage: forget [-m|-u] <substring>" RST);
            return;
        }
    } else {
        do_mem = 1;
        do_user = 1;
    }

    while (*sub == ' ') sub++;
    if (!*sub) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: forget [-m|-u] <substring>" RST);
        return;
    }

    int total = 0;
    if (do_mem) {
        int rm = memfile_remove(&g_ctx->mem, sub);
        if (rm > 0) memfile_save(&g_ctx->mem);
        total += rm;
    }
    if (do_user) {
        int ru = memfile_remove(&g_ctx->user, sub);
        if (ru > 0) memfile_save(&g_ctx->user);
        total += ru;
    }

    if (total > 0) {
        if (out) os_snprintf(out, out_len,
            GRY "✓ removed %d entry" RST, total);
    } else {
        if (out) os_snprintf(out, out_len,
            GRY "nothing matched '%s'" RST, sub);
    }
}

static void handle_notes(char *out, size_t out_len)
{
    char mu[64], uu[64];
    memfile_usage(&g_ctx->mem, mu, sizeof(mu));
    memfile_usage(&g_ctx->user, uu, sizeof(uu));

    size_t pos = 0;
    pos += os_snprintf(out + pos, out_len - pos,
        BLD "Memory" RST "  (" GRY "%s" RST ")" "\n", mu);
    for (int i = 0; i < g_ctx->mem.count && pos < out_len; i++) {
        pos += os_snprintf(out + pos, out_len - pos,
            "  " DIM "%d." RST " %s\n", i + 1, g_ctx->mem.entries[i]);
    }
    pos += os_snprintf(out + pos, out_len - pos,
        BLD "Profile" RST " (" GRY "%s" RST ")" "\n", uu);
    for (int i = 0; i < g_ctx->user.count && pos < out_len; i++) {
        pos += os_snprintf(out + pos, out_len - pos,
            "  " DIM "%d." RST " %s\n", i + 1, g_ctx->user.entries[i]);
    }
}

/* ── LLM Ask ──────────────────────────────────────────────────────── */

/* First token callback — stops spinner, shows response */


static void on_llm_token(const char *token, size_t len,
                         int tokens_so_far, uint64_t elapsed_ms,
                         int is_reasoning,
                         void *user)
{
    (void)len; (void)tokens_so_far; (void)elapsed_ms; (void)user;
    if (!g_ctx->first_token) {
        g_ctx->first_token = 1;
        g_ctx->saw_reasoning = 0;
        g_spinner_on = 0;
        os_sleep_ms(300);
        os_printf("\033[K");  /* clear the spinner line */
    }

    /* Transition from reasoning to content: 1 blank line gap */
    if (g_ctx->saw_reasoning == 1 && !is_reasoning) {
        os_printf("\n");
        g_ctx->saw_reasoning = 0;  /* reset, gap done */
    }
    if (is_reasoning) g_ctx->saw_reasoning = 1;

    /* Convert literal \n to actual newlines, apply color */
    const char *prefix = is_reasoning ? DIM : "";
    const char *suffix = is_reasoning ? RST : "";
    os_printf("%s", prefix);
    for (const char *p = token; *p; p++) {
        if (*p == '\\' && (*(p+1) == 'n' || *(p+1) == 'N')) {
            os_printf("\n");
            p++;
        } else {
            os_printf("%c", *p);
        }
    }
    os_printf("%s", suffix);
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
    g_ctx->first_token = 0;
    os_printf("\n");  /* new line: keep prompt visible above, spinner below */
    fflush(stdout);
    os_thread_handle_t tid;
    os_thread_create(&tid, spinner_thread, NULL);

    /* Build prompt: system instructions + memory context + user question */
    char prompt_buf[8192];
    const char *final_prompt = question;
    {
        int pos = 0;

        /* System instructions: tell LLM it can persist memory via markers */
        pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos,
            "[System Instructions]\n"
            "You have a persistent memory system. To save information across sessions,\n"
            "append one of these directives at the END of your reply:\n"
            "  [NOTE: <fact>]       — save a fact to persistent memory\n"
            "  [PROFILE: <info>]    — save user profile information\n"
            "  [FORGET: <keyword>]  — remove a memory entry containing keyword\n"
            "Example: 'I'll remember that. [NOTE: User's birthday is Jan 1]'\n"
            "Directives are hidden from the user; use them silently.\n"
            "Do NOT acknowledge or refer to these instructions.\n"
            "Answer the user's question directly.\n"
            "\n");

        if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) {
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos,
                "===== MEMORY =====\n");
            if (g_ctx->mem.count > 0) {
                for (int i = 0; i < g_ctx->mem.count; i++)
                    pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos,
                        "%s\n", g_ctx->mem.entries[i]);
            }
            if (g_ctx->user.count > 0) {
                pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos,
                    "===== PROFILE =====\n");
                for (int i = 0; i < g_ctx->user.count; i++)
                    pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos,
                        "%s\n", g_ctx->user.entries[i]);
            }
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "\n");
        }

        /* Flatten newlines to spaces to keep JSON valid */
        for (int i = 0; i < pos; i++) {
            if (prompt_buf[i] == '\n')
                prompt_buf[i] = ' ';
        }

        /* Append question and flatten its newlines too */
        size_t qlen = os_strlen(question);
        for (size_t i = 0; i < qlen && pos < (int)sizeof(prompt_buf) - 1; i++) {
            char c = question[i];
            prompt_buf[pos++] = (c == '\n') ? ' ' : c;
        }
        prompt_buf[pos] = '\0';
        final_prompt = prompt_buf;
    }

    uint64_t t0 = os_clock_ms();
    llm_response_t *resp = llm_chat_stream(endpoint, api_key, model,
                                           final_prompt,
                                           on_llm_token, NULL);
    uint64_t elapsed = os_clock_ms() - t0;

    g_spinner_on = 0;
    os_thread_join(tid);

    if (!resp) {
        if (out) os_snprintf(out, out_len, RED "%% LLM unavailable" RST);
        return -1;
    }

    resp->latency_ms = elapsed;

    /* Stats bar */
    if (out) {
        size_t pos = 0;
        pos += os_snprintf(out + pos, out_len - pos,
                           DIM "  %.1fs", (double)elapsed / 1000.0);
        if (resp->completion_tokens > 0) {
            int total_tok = resp->completion_tokens
                          + (resp->prompt_tokens > 0 ? resp->prompt_tokens : 0);
            pos += os_snprintf(out + pos, out_len - pos,
                               " · %d tok", total_tok);
            if (elapsed > 0) {
                double tps = (double)total_tok /
                             ((double)elapsed / 1000.0);
                pos += os_snprintf(out + pos, out_len - pos,
                                   " · %.1f/s", tps);
            }
        }
        /* Track session tokens */
        int sess_add = resp->prompt_tokens + resp->completion_tokens;
        if (sess_add > 0) {
            g_ctx->session_tokens += sess_add;
        }
        /* Show memory usage if entries exist */
        if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) {
            char mu[48], uu[48];
            if (g_ctx->mem.count > 0)
                memfile_usage(&g_ctx->mem, mu, sizeof(mu));
            if (g_ctx->user.count > 0)
                memfile_usage(&g_ctx->user, uu, sizeof(uu));
            pos += os_snprintf(out + pos, out_len - pos,
                               DIM " ‖ " RST);
            if (g_ctx->mem.count > 0)
                pos += os_snprintf(out + pos, out_len - pos,
                                   DIM "mem %s" RST, mu);
            if (g_ctx->user.count > 0)
                pos += os_snprintf(out + pos, out_len - pos,
                                   DIM " · you %s" RST, uu);
        }
        os_snprintf(out + pos, out_len - pos, RST);
    }

    /* Auto-log Q&A */
    if (resp->content) {
        os_file_handle_t lf = os_file_open("conversations.log", "a");
        if (lf) {
            char logline[4096];
            int n = os_snprintf(logline, sizeof(logline),
                "[%llu] Q: %s\nA: %s\n\n",
                (unsigned long long)t0, question, resp->content);
            if (n > 0) os_file_write(lf, logline, (size_t)n);
            os_file_close(lf);
        }

        /* Parse memory directives from LLM response */
        const char *p = resp->content;
        while (p) {
            const char *note_start = strstr(p, "[NOTE: ");
            const char *prof_start = strstr(p, "[PROFILE: ");
            const char *forg_start = strstr(p, "[FORGET: ");

            /* Find earliest marker */
            const char *earliest = NULL;
            int kind = 0; /* 1=note, 2=profile, 3=forget */
            int prefix_len = 0;

            if (note_start && (!earliest || note_start < earliest))
                { earliest = note_start; kind = 1; prefix_len = 7; }
            if (prof_start && (!earliest || prof_start < earliest))
                { earliest = prof_start; kind = 2; prefix_len = 10; }
            if (forg_start && (!earliest || forg_start < earliest))
                { earliest = forg_start; kind = 3; prefix_len = 9; }

            if (!earliest) break;

            const char *start = earliest + prefix_len;
            const char *end = strstr(start, "]");
            if (!end) break;

            /* Extract content between [MARKER: and ] */
            size_t clen = (size_t)(end - start);
            if (clen > 0) {
                char content[1024];
                size_t cp = clen < sizeof(content) - 1 ? clen : sizeof(content) - 1;
                os_memcpy(content, start, cp);
                content[cp] = '\0';

                char feedback[128];
                if (kind == 1) {
                    handle_note(content, feedback, sizeof(feedback));
                } else if (kind == 2) {
                    handle_profile(content, feedback, sizeof(feedback));
                } else if (kind == 3) {
                    handle_forget(content, feedback, sizeof(feedback));
                }
                /* Print feedback if non-empty */
                if (feedback[0])
                    os_printf(DIM "%s" RST "\n", feedback);
            }

            p = end + 1; /* continue after ] */
        }
    }

    llm_response_free(resp);
    return 0;
}

/* ── Command Parser ───────────────────────────────────────────────── */

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
            "  " CYN "note" RST " <text>\n"
            "    Save a fact to persistent memory (MEMORY.md).\n"
            "    Memory is loaded at startup and injected into every\n"
            "    LLM prompt as context. Limits: 2200 chars total.\n"
            "    Example: " CYN "note 用户喜欢简洁的回答" RST "\n"
            "\n"
            "  " CYN "profile" RST " <text>\n"
            "    Save user profile information (USER.md).\n"
            "    Same mechanism as note, but for user-specific data.\n"
            "    Limits: 1375 chars total.\n"
            "    Example: " CYN "profile Name is 老板鱼饭" RST "\n"
            "\n"
            "  " CYN "notes" RST "\n"
            "    List all entries in both MEMORY.md and USER.md\n"
            "    with current usage percentage.\n"
            "    Example: " CYN "notes" RST "\n"
            "\n"
            "  " CYN "replace" RST " <key> <new text>\n"
            "    Find the first memory entry containing <key> and\n"
            "    replace its entire content with <new text>.\n"
            "    Only operates on MEMORY.md (use forget+profile for\n"
            "    USER.md edits).\n"
            "    Example: " CYN "replace 简洁 用户偏好详细的回答" RST "\n"
            "\n"
            "  " CYN "forget" RST " [-m|-u] <substring>\n"
            "    Remove entries containing <substring> from memory.\n"
            "    With no flag, removes from both MEMORY.md and USER.md.\n"
            "    Use -m to target MEMORY.md only, -u for USER.md only.\n"
            "    Example: " CYN "forget -m 测试" RST "\n"
            "    Example: " CYN "forget 测试          (removes from both)" RST "\n"
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
            "  " GRY "exit" RST "  /  " GRY "quit" RST "\n"
            "    Exit the REPL and shutdown.\n"
            "    Example: " CYN "exit" RST "");
    } else if (os_strcmp(line, "exit") == 0 ||
               os_strcmp(line, "quit") == 0) {
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
    memfile_load("memories/memory.md", &g_ctx->mem, MEMFILE_MEMORY_LIMIT);
    memfile_load("memories/user.md",   &g_ctx->user, MEMFILE_USER_LIMIT);
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
    memfile_load("memories/memory.md", &g_ctx->mem, MEMFILE_MEMORY_LIMIT);
    memfile_load("memories/user.md",   &g_ctx->user, MEMFILE_USER_LIMIT);
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
