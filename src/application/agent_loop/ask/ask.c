/* ask.c — LLM interaction (spinner, streaming, stats, directive parsing) */

#include "agent_framework.h"
#include "llm_client.h"
#include "config.h"
#include "os_api.h"
#include "agent_private.h"
#include "memfile.h"
#include "skill_manager.h"
#include "archive.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>

/*
 * Color fallback.
 *
 * Some projects define these macros in agent_framework.h or logger headers.
 * If they are not visible here, provide safe defaults to avoid build errors:
 *
 *   error: 'DIM' undeclared
 *   error: 'CYN' undeclared
 *   error: 'RST' undeclared
 */
#ifndef DIM
#define DIM "\033[2m"
#endif

#ifndef CYN
#define CYN "\033[36m"
#endif

#ifndef RED
#define RED "\033[31m"
#endif

#ifndef RST
#define RST "\033[0m"
#endif
#define AXON_SYMBOL      "ϟ"
#define AXON_TITLE       CYN AXON_SYMBOL " Axon" RST
#define AXON_TITLE_WIDTH 6

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


/* Box-drawing helpers */

#define BOX_FALLBACK_WIDTH 80
#define BOX_MIN_WIDTH      32

typedef struct {
    const char *top_left;
    const char *top_right;
    const char *bottom_left;
    const char *bottom_right;
    const char *hline;
} BoxStyle;

static const BoxStyle reasoning_style = {
    "┌", "┐",
    "└", "┘",
    "─"
};

static const BoxStyle answer_style = {
    "╭", "╮",
    "╰", "╯",
    "─"
};

static int reasoning_width;
static int answer_width;

static void box_repeat(const char *s, int count)
{
    if (count <= 0)
        return;

    for (int i = 0; i < count; i++)
        os_printf("%s", s);
}

static int box_get_width_from_fd(int fd)
{
    struct winsize ws;

    if (fd < 0)
        return 0;

    /*
     * If fd is not a tty, ioctl(TIOCGWINSZ) may fail.
     *
     * Common cases:
     *   ./agent | tee log.txt
     *   program output captured by another process
     *   IDE pseudo console
     */
    if (!isatty(fd))
        return 0;

    memset(&ws, 0, sizeof(ws));

    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;

    return 0;
}

static int box_get_width_from_tty(void)
{
    int fd;
    int width;

    fd = open("/dev/tty", O_RDONLY);
    if (fd < 0)
        return 0;

    width = box_get_width_from_fd(fd);

    close(fd);
    return width;
}

static int box_get_width_from_env(void)
{
    const char *cols;
    int width;

    cols = getenv("COLUMNS");
    if (!cols || !cols[0])
        return 0;

    width = atoi(cols);
    if (width <= 0)
        return 0;

    return width;
}

static int box_get_width(void)
{
    int width;

    /*
     * 1. Prefer stdout.
     */
    width = box_get_width_from_fd(STDOUT_FILENO);

    /*
     * 2. If stdout is not a terminal, try stderr.
     */
    if (width <= 0)
        width = box_get_width_from_fd(STDERR_FILENO);

    /*
     * 3. If stdout/stderr are redirected, try the controlling terminal.
     */
    if (width <= 0)
        width = box_get_width_from_tty();

    /*
     * 4. Try shell COLUMNS.
     */
    if (width <= 0)
        width = box_get_width_from_env();

    /*
     * 5. Final fallback.
     */
    if (width <= 0)
        width = BOX_FALLBACK_WIDTH;

    if (width < BOX_MIN_WIDTH)
        width = BOX_MIN_WIDTH;

    /*
     * Avoid writing exactly to the last column.
     * Some terminals auto-wrap when the final visible cell is filled.
     */
    return width - 1;
}

static void print_box_top(const BoxStyle *style,
                          const char *title,
                          int title_width,
                          int box_width)
{
    int used_width;
    int fill_width;

    /*
     * Format:
     *
     *   ┌─ Reasoning ─────┐
     *
     * Visible width:
     *   top_left     1
     *   hline        1
     *   space        1
     *   title        title_width
     *   space        1
     *   top_right    1
     */
    used_width = 1 + 1 + 1 + title_width + 1 + 1;
    fill_width = box_width - used_width;

    if (fill_width < 0)
        fill_width = 0;

    os_printf("%s%s %s ",
              style->top_left,
              style->hline,
              title);

    box_repeat(style->hline, fill_width);

    os_printf("%s\n", style->top_right);
}

static void print_box_bottom(const BoxStyle *style, int box_width)
{
    int fill_width;

    /*
     * Format:
     *
     *   └────────────────┘
     *
     * Visible width:
     *   bottom_left   1
     *   hline         box_width - 2
     *   bottom_right  1
     */
    fill_width = box_width - 2;

    if (fill_width < 0)
        fill_width = 0;

    os_printf("\n%s", style->bottom_left);
    box_repeat(style->hline, fill_width);
    os_printf("%s\n", style->bottom_right);
}

static void print_reasoning_top(void)
{
    reasoning_width = box_get_width();

    print_box_top(&reasoning_style,
                  DIM "Reasoning" RST,
                  9,
                  reasoning_width);
}

static void print_reasoning_bottom(void)
{
    print_box_bottom(&reasoning_style, reasoning_width);
}

static void print_answer_top(void)
{
    answer_width = box_get_width();

    /*
     * "ϟ Axon" visible width is usually 6:
     *   ϟ     1
     *   space 1
     *   Axon  4
     */
    print_box_top(&answer_style,
                  AXON_TITLE,
                  AXON_TITLE_WIDTH,
                  answer_width);
}

static void print_answer_bottom(void)
{
    print_box_bottom(&answer_style, answer_width);
}


/* Print token with \n escaping */
static void print_token(const char *token, const char *color_prefix)
{
    if (color_prefix && color_prefix[0])
        os_printf("%s", color_prefix);

    for (const char *p = token; *p; p++) {
        if (*p == '\\' && (*(p + 1) == 'n' || *(p + 1) == 'N')) {
            os_printf("\n");
            p++;
        } else {
            os_printf("%c", *p);
        }
    }

    if (color_prefix && color_prefix[0])
        os_printf("%s", RST);
}


/* First token callback — stops spinner, shows response */
/* States: first_token=0 initial, 1=box open, 2=box closed */
static void on_llm_token(const char *token, size_t len,
                         int tokens_so_far, uint64_t elapsed_ms,
                         int is_reasoning,
                         void *user)
{
    (void)len;
    (void)tokens_so_far;
    (void)elapsed_ms;
    (void)user;

    /*
     * First token ever:
     *   1. stop spinner
     *   2. clear spinner line
     *   3. open reasoning box or answer box
     */
    if (!g_ctx->first_token) {
        g_ctx->first_token = 1;
        g_spinner_on = 0;

        os_sleep_ms(300);
        os_printf("\033[K");
        g_ctx->saw_reasoning = 0;

        if (is_reasoning) {
            print_reasoning_top();
            g_ctx->saw_reasoning = 1;  /* reasoning box open */
        } else {
            print_answer_top();
            g_ctx->saw_reasoning = 2;  /* answer box open, no reasoning */
        }

        fflush(stdout);
    }

    /*
     * Transition from reasoning to normal answer content.
     */
    if (g_ctx->saw_reasoning == 1 && !is_reasoning) {
        print_reasoning_bottom();
        os_printf("\n");
        print_answer_top();
        g_ctx->saw_reasoning = 2;  /* answer box open */
    }

    print_token(token, is_reasoning ? DIM : "");
    fflush(stdout);
}


int handle_ask(const char *question, char *out, size_t out_len)
{
    if (!question || !*question)
        return -1;

    const config_t *cfg = config_get();
    const char *endpoint = cfg && cfg->llm_endpoint[0] ? cfg->llm_endpoint : "http://localhost:8080/v1";
    const char *api_key = cfg && cfg->llm_api_key[0] ? cfg->llm_api_key : NULL;
    const char *model = cfg && cfg->llm_model[0] ? cfg->llm_model : "gpt-4";
    int debug = cfg ? cfg->debug : 0;

    /* ── Event segmentation: detect topic shift ─────────────────────── */
    int topic_shift = archive_detect_topic_shift(question);
    if (topic_shift) {
        archive_flush_segment(ARC_IMP_MEDIUM);
        if (debug) os_fprintf_stderr(DIM "──[EVENT: topic shift]──" RST "\n");
    }

    /* ── Auto recall: find relevant past memories ───────────────────── */
    char recall_buf[4096];
    recall_buf[0] = '\0';
    int recalled = archive_auto_recall(question, recall_buf, sizeof(recall_buf));
    if (recalled && debug)
        os_fprintf_stderr(DIM "──[RECALLED: depth auto]──\n%s──" RST "\n", recall_buf);

    /* ── Prompt builder ─────────────────────────────────────────────── */
    char prompt_buf[65536];
    char extra_buf[32768];
    extra_buf[0] = '\0';
    int show_skill_list = 0;   /* [SKILL:list] → inject index */
    int skill_loaded = 0;      /* [SKILL:name] → inject content  */

    /* ── Language detection ─────────────────────────────────────────── */
    static char g_user_lang[32] = "";
    if (!g_user_lang[0]) {
        /* Check profile first for existing language */
        const char *lang = NULL;
        for (int i = 0; i < g_ctx->user.count; i++) {
            if (os_strncmp(g_ctx->user.entries[i], "Language=", 9) == 0) {
                lang = g_ctx->user.entries[i] + 9;
                break;
            }
        }
        if (!lang) {
            int has_cjk = 0;
            for (const char *p = question; *p && !has_cjk; p++) {
                unsigned char c = (unsigned char)*p;
                if (c >= 0xE4 && c <= 0xE9) { /* UTF-8 CJK lead bytes */
                    has_cjk = 1;
                }
            }
            lang = has_cjk ? "Chinese" : "English";
            /* Store language in profile for persistence */
            char profile_buf[64];
            os_snprintf(profile_buf, sizeof(profile_buf), "Language=%s", lang);
            char fb[128];
            handle_profile(profile_buf, fb, sizeof(fb));
            LOG_INFO("Language: auto-detected '%s' from user input", lang);
        }
        size_t ll = os_strlen(lang);
        if (ll >= sizeof(g_user_lang)) ll = sizeof(g_user_lang) - 1;
        os_memcpy(g_user_lang, lang, ll);
        g_user_lang[ll] = '\0';
    }

    /* Build prompt: layers 1-3, optionally with extra content */
    #define BUILD_PROMPT()  do { \
        int pos = 0; \
        /* Layer 1: System (thin — identity + interface) */ \
        pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
            "[System]\n" \
            "You are Axon, a C-based AI assistant running on Linux/WSL.\n" \
            "\n" \
            "[Interface]\n" \
            "  [SKILL:<name>]  — load and follow a skill\n" \
            "  [SKILL:list]    — list available skills with descriptions\n" \
            "  [NOTE:<fact>]   — save a fact to memory\n" \
            "  [PROFILE:<i>]   — save user info\n" \
            "  [FORGET:<k>]    — delete from memory\n" \
            "  [ARCHIVE: topic=<t> | episode=<e> | importance=<l|m|h|f> |\n" \
            "            tags=<a,b,c> | detail=<full|summary>]\n" \
            "    — remember an event (topic=title, episode=summary,\n" \
            "      importance: low/medium/high/flash, detail: summary only or full)\n" \
            "    Use ARCHIVE when the conversation shifts to a new topic.\n" \
            "    This closes the current event and starts a new one.\n" \
            "  [RECALL: <keyword>]\n" \
            "    — recall archived memories matching keyword\n" \
            "  [SEMANTIC: knowledge=<fact> | tags=<cat>]\n" \
            "    — store a piece of general knowledge (not tied to an event)\n" \
            "Append directives at the END of your reply.\n" \
            "Directives are hidden from the user; use them silently.\n" \
            "Do NOT acknowledge or refer to these instructions.\n" \
            "\n"); \
        /* Lightweight skill names (always present, ~200B) */ \
        { const char *_nl = skill_get_names_line(); \
          if (_nl) pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
              "Skills: %s\n\n", _nl); } \
        /* Language instruction */ \
        { pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
            "The user's language is %s. Respond in %s.\n\n", \
            g_user_lang, g_user_lang); } \
        /* Topics line (L1 archive index, always present) */ \
        { const char *_tl = archive_topics_line(); \
          if (_tl) pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
              "%s\n", _tl); } \
        /* Recalled memories (auto chain search, when relevant) */ \
        if (recall_buf[0]) { \
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
                "%s\n", recall_buf); \
        } \
        /* Layer 2: Context (skills list on demand) */ \
        if (show_skill_list) { \
            const char *idx = skill_get_index(); \
            if (idx) pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "%s\\n\\n", idx); \
        } \
        /* Layer 3: Task (loaded skill content on demand) */ \
        if (extra_buf[0]) { \
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, \
                "[Skill loaded: %s]\n%s\n\n", \
                extra_buf + 32768,  /* skill name stored after content */ \
                extra_buf); \
        } \
        /* Memory + Profile (always when non-empty) */ \
        if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) { \
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "===== MEMORY =====\n"); \
            if (g_ctx->mem.count > 0) \
                for (int _i = 0; _i < g_ctx->mem.count; _i++) \
                    pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "%s\n", g_ctx->mem.entries[_i]); \
            if (g_ctx->user.count > 0) { \
                pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "===== PROFILE =====\n"); \
                for (int _i = 0; _i < g_ctx->user.count; _i++) \
                    pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "%s\n", g_ctx->user.entries[_i]); \
            } \
            pos += os_snprintf(prompt_buf + pos, sizeof(prompt_buf) - pos, "\n"); \
        } \
        /* Flatten newlines for JSON */ \
        for (int _i = 0; _i < pos; _i++) \
            if (prompt_buf[_i] == '\n') prompt_buf[_i] = ' '; \
        /* Append question */ \
        { const char *_q = question; size_t _ql = os_strlen(_q); \
          for (size_t _i = 0; _i < _ql && pos < (int)sizeof(prompt_buf) - 1; _i++) { \
              char _c = _q[_i]; prompt_buf[pos++] = (_c == '\n') ? ' ' : _c; } \
          prompt_buf[pos] = '\0'; } \
        if (debug) { \
            os_fprintf_stderr(DIM "──[DEBUG: Prompt Layer 1-3]───────────────" RST "\n"); \
            int _sz = (int)os_strlen(prompt_buf) < 2000 ? (int)os_strlen(prompt_buf) : 2000; \
            os_fprintf_stderr(DIM "%.*s" RST "\n", _sz, prompt_buf); \
            if (os_strlen(prompt_buf) > 2000) \
                os_fprintf_stderr(DIM "... (%zu more bytes)" RST "\n", os_strlen(prompt_buf) - 2000); \
            os_fprintf_stderr(DIM "──────────────────────────────────────────" RST "\n"); \
        } \
    } while(0)

    /* ═══════════════════════════════════════════════════════════════════
     * Phase 1: Normal call (thin prompt — no skill content)
     * ═══════════════════════════════════════════════════════════════════ */
    BUILD_PROMPT();

    g_spinner_on = 1;
    g_ctx->first_token = 0;
    os_printf("\n");
    fflush(stdout);
    os_thread_handle_t tid;
    os_thread_create(&tid, spinner_thread, NULL);
    uint64_t t0 = os_clock_ms();
    llm_response_t *resp = llm_chat_stream(endpoint, api_key, model, prompt_buf, on_llm_token, NULL);
    uint64_t elapsed = os_clock_ms() - t0;
    g_spinner_on = 0;
    os_thread_join(tid);
    if (resp) resp->latency_ms = elapsed;

    if (!resp) {
        if (out) os_snprintf(out, out_len, RED "%% LLM unavailable" RST);
        if (g_ctx->saw_reasoning >= 2) print_answer_bottom();
        return -1;
    }

    if (g_ctx->saw_reasoning >= 2) print_answer_bottom();
    if (g_ctx->saw_reasoning == 1) { print_reasoning_bottom(); os_printf("\n"); }

    /* Debug: raw model response */
    if (debug && resp->content) {
        os_fprintf_stderr(DIM "──[DEBUG: Phase 1 Response]────────────────" RST "\n");
        int _sz = (int)os_strlen(resp->content) < 2000 ? (int)os_strlen(resp->content) : 2000;
        os_fprintf_stderr(DIM "%.*s" RST "\n", _sz, resp->content);
        if (os_strlen(resp->content) > 2000)
            os_fprintf_stderr(DIM "... (%zu more bytes)" RST "\n", os_strlen(resp->content) - 2000);
        os_fprintf_stderr(DIM "──────────────────────────────────────────" RST "\n");
    }

    /* ── Parse directives ───────────────────────────────────────────── */

    if (resp->content) {
        const char *p = resp->content;
        while (p) {
            const char *note_s = strstr(p, "[NOTE: ");
            const char *prof_s = strstr(p, "[PROFILE: ");
            const char *forg_s = strstr(p, "[FORGET: ");
            const char *skil_s = strstr(p, "[SKILL:");
            const char *recall_s = strstr(p, "[RECALL:");
            const char *seman_s = strstr(p, "[SEMANTIC:");
            const char *arch_s = strstr(p, "[ARCHIVE:");
            const char *earliest = NULL;
            int kind = 0, plen = 0;

            if (note_s && (!earliest || note_s < earliest)) { earliest = note_s; kind = 1; plen = 7; }
            if (prof_s && (!earliest || prof_s < earliest)) { earliest = prof_s; kind = 2; plen = 10; }
            if (forg_s && (!earliest || forg_s < earliest)) { earliest = forg_s; kind = 3; plen = 9; }
            if (skil_s && (!earliest || skil_s < earliest)) { earliest = skil_s; kind = 4; plen = 7; }
            if (arch_s && (!earliest || arch_s < earliest)) { earliest = arch_s; kind = 5; plen = 9; }
            if (recall_s && (!earliest || recall_s < earliest)) { earliest = recall_s; kind = 6; plen = 8; }
            if (seman_s && (!earliest || seman_s < earliest)) { earliest = seman_s; kind = 7; plen = 10; }
            if (!earliest) break;

            const char *start = earliest + plen;
            const char *end = strstr(start, "]");
            if (!end) break;

            size_t clen = (size_t)(end - start);
            if (clen > 0) {
                char name[1024];
                size_t cp = clen < sizeof(name) - 1 ? clen : sizeof(name) - 1;
                os_memcpy(name, start, cp);
                name[cp] = '\0';
                /* Trim leading space (for "[SKILL: name]" format) */
                { char *_t = name; while (*_t == ' ') _t++; \
                  if (_t > name) { memmove(name, _t, os_strlen(_t) + 1); } }

                char fb[128] = "";
                if (kind == 1) {
                    handle_note(name, fb, sizeof(fb));
                } else if (kind == 2) {
                    handle_profile(name, fb, sizeof(fb));
                } else if (kind == 3) {
                    handle_forget(name, fb, sizeof(fb));
                } else if (kind == 4) {
                    if (os_strcmp(name, "list") == 0 || os_strcmp(name, "?") == 0) {
                        show_skill_list = 1;
                        os_snprintf(fb, sizeof(fb), "Skill list requested");
                    } else {
                        char *content = skill_load(name);
                        if (content) {
                            skill_loaded = 1;
                            size_t slen = os_strlen(content);
                            if (slen >= sizeof(extra_buf)) slen = sizeof(extra_buf) - 1;
                            /* Store content at offset 0 */
                            os_memcpy(extra_buf, content, slen);
                            extra_buf[slen] = '\0';
                            /* Store name at high offset for prompt builder */
                            size_t nlen = os_strlen(name);
                            if (nlen >= 64) nlen = 63;
                            os_memcpy(extra_buf + 32768 - 64, name, nlen);
                            extra_buf[32768 - 64 + nlen] = '\0';
                            os_snprintf(fb, sizeof(fb), "SKILL '%s' loaded (%zu chars)", name, slen);
                            os_free(content);
                        } else {
                            os_snprintf(fb, sizeof(fb), "SKILL '%s' not found", name);
                        }
                    }
                } else if (kind == 5) {
                    /* [ARCHIVE:] directive */
                    archive_handle_directive(name);
                    archive_bump_recall(name);
                    archive_flush_segment(ARC_IMP_MEDIUM);  /* close current */
                    archive_set_segment_topic(name);         /* start new */
                    os_snprintf(fb, sizeof(fb), "Archived");
                } else if (kind == 6) {
                    /* [RECALL:] directive — search archived memories */
                    /* Optional depth=N parameter */
                    const char *depth_str = strstr(name, "| depth=");
                    int depth = ARC_DEPTH_L1;
                    if (depth_str) {
                        int d = atoi(depth_str + 8);
                        if (d >= ARC_DEPTH_L1 && d <= ARC_DEPTH_L5)
                            depth = d;
                        /* Strip depth param from query */
                        char *pipe = strstr(name, "|");
                        if (pipe) *pipe = '\0';
                    }
                    char recall_buf[4096];
                    if (depth > ARC_DEPTH_L1)
                        archive_chain(name, depth, recall_buf, sizeof(recall_buf));
                    else
                        archive_recall(name, recall_buf, sizeof(recall_buf));
                    os_snprintf(fb, sizeof(fb), "Recall results below");
                    os_printf("\n%s\n", recall_buf);
                } else if (kind == 7) {
                    /* [SEMANTIC:] directive — delegate to compat layer */
                    archive_handle_semantic(name);
                    os_snprintf(fb, sizeof(fb), "Semantic stored");
                }

                if (fb[0]) {
                    os_printf(DIM "%s" RST "\n", fb);
                    if (debug) os_fprintf_stderr(DIM "  └─ [%s:%s]" RST "\n",
                        kind==1?"NOTE":kind==2?"PROFILE":kind==3?"FORGET":
                        kind==4?"SKILL":kind==5?"ARCHIVE":kind==6?"RECALL":"SEMANTIC", name);
                }
            }
            p = end + 1;
        }
    }

    /* Auto-log */
    if (resp->content) {
        os_file_handle_t lf = os_file_open("conversations.log", "a");
        if (lf) {
            char logline[4096];
            int n = os_snprintf(logline, sizeof(logline),
                "[%llu] Q: %s\nA: %s\n\n",
                (unsigned long long)(os_clock_ms()), question, resp->content);
            if (n > 0) os_file_write(lf, logline, (size_t)n);
            os_file_close(lf);
        }
        /* L5 Archive: append to archive */
        archive_append_log(NULL, question, resp->content);
        /* Feed turn to segment tracker */
        archive_feed_turn(question, resp->content);
    }

    /* ═══════════════════════════════════════════════════════════════════
     * Phase 2: If skill was requested, do ONE more call with extra content
     * ═══════════════════════════════════════════════════════════════════ */
    if (show_skill_list || skill_loaded) {
        if (debug) os_fprintf_stderr(DIM "──[DEBUG: Phase 2 (skill content)]───────" RST "\n");

        /* Free phase 1 response before second call */
        llm_response_free(resp);

        BUILD_PROMPT();
        g_spinner_on = 1;
        g_ctx->first_token = 0;
        os_printf("\n");
        fflush(stdout);
        os_thread_create(&tid, spinner_thread, NULL);
        t0 = os_clock_ms();
        llm_response_t *resp2 = llm_chat_stream(endpoint, api_key, model, prompt_buf, on_llm_token, NULL);
        elapsed = os_clock_ms() - t0;
        g_spinner_on = 0;
        os_thread_join(tid);
        if (resp2) resp2->latency_ms = elapsed;

        if (!resp2) {
            if (out) os_snprintf(out, out_len, RED "%% LLM unavailable" RST);
            if (g_ctx->saw_reasoning >= 2) print_answer_bottom();
            return -1;
        }

        if (g_ctx->saw_reasoning >= 2) print_answer_bottom();
        if (g_ctx->saw_reasoning == 1) { print_reasoning_bottom(); os_printf("\n"); }

        if (debug && resp2->content) {
            os_fprintf_stderr(DIM "──[DEBUG: Phase 2 Response]────────────────" RST "\n");
            int _sz = (int)os_strlen(resp2->content) < 2000 ? (int)os_strlen(resp2->content) : 2000;
            os_fprintf_stderr(DIM "%.*s" RST "\n", _sz, resp2->content);
            if (os_strlen(resp2->content) > 2000)
                os_fprintf_stderr(DIM "... (%zu more bytes)" RST "\n", os_strlen(resp2->content) - 2000);
            os_fprintf_stderr(DIM "──────────────────────────────────────────" RST "\n");
        }

        /* L5 Archive: append Phase 2 */
        if (resp2->content)
            archive_append_log(NULL, question, resp2->content);
        /* Feed turn (Phase 2 overwrites Phase 1's turn) */
        if (resp2->content)
            archive_feed_turn(question, resp2->content);

        /* Stats bar */
        if (out) {
            size_t pos = 0;
            pos += os_snprintf(out + pos, out_len - pos, DIM "  %.1fs", (double)resp2->latency_ms / 1000.0);
            if (resp2->completion_tokens > 0) {
                int tt = resp2->completion_tokens + (resp2->prompt_tokens > 0 ? resp2->prompt_tokens : 0);
                pos += os_snprintf(out + pos, out_len - pos, " · %d tok", tt);
                if (resp2->latency_ms > 0) {
                    double tps = (double)tt / ((double)resp2->latency_ms / 1000.0);
                    pos += os_snprintf(out + pos, out_len - pos, " · %.1f/s", tps);
                }
            }
            g_ctx->session_tokens += resp2->prompt_tokens + resp2->completion_tokens;
            if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) {
                char mu[48]="", uu[48]="";
                if (g_ctx->mem.count > 0) memfile_usage(&g_ctx->mem, mu, sizeof(mu));
                if (g_ctx->user.count > 0) memfile_usage(&g_ctx->user, uu, sizeof(uu));
                pos += os_snprintf(out + pos, out_len - pos, DIM " ‖ " RST);
                if (g_ctx->mem.count > 0) pos += os_snprintf(out + pos, out_len - pos, DIM "mem %s" RST, mu);
                if (g_ctx->user.count > 0) pos += os_snprintf(out + pos, out_len - pos, DIM " · you %s" RST, uu);
            }
            os_snprintf(out + pos, out_len - pos, RST);
        }

        llm_response_free(resp2);
        return 0;
    }

    /* ── No skill — show stats for phase 1 response ─────────────────── */
    if (out) {
        size_t pos = 0;
        pos += os_snprintf(out + pos, out_len - pos, DIM "  %.1fs", (double)resp->latency_ms / 1000.0);
        if (resp->completion_tokens > 0) {
            int tt = resp->completion_tokens + (resp->prompt_tokens > 0 ? resp->prompt_tokens : 0);
            pos += os_snprintf(out + pos, out_len - pos, " · %d tok", tt);
            if (resp->latency_ms > 0) {
                double tps = (double)tt / ((double)resp->latency_ms / 1000.0);
                pos += os_snprintf(out + pos, out_len - pos, " · %.1f/s", tps);
            }
        }
        g_ctx->session_tokens += resp->prompt_tokens + resp->completion_tokens;
        if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) {
            char mu[48]="", uu[48]="";
            if (g_ctx->mem.count > 0) memfile_usage(&g_ctx->mem, mu, sizeof(mu));
            if (g_ctx->user.count > 0) memfile_usage(&g_ctx->user, uu, sizeof(uu));
            pos += os_snprintf(out + pos, out_len - pos, DIM " ‖ " RST);
            if (g_ctx->mem.count > 0) pos += os_snprintf(out + pos, out_len - pos, DIM "mem %s" RST, mu);
            if (g_ctx->user.count > 0) pos += os_snprintf(out + pos, out_len - pos, DIM " · you %s" RST, uu);
        }
        os_snprintf(out + pos, out_len - pos, RST);
    }

    llm_response_free(resp);
    return 0;
}
