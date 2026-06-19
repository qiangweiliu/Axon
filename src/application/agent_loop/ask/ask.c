/* ask.c — LLM interaction (spinner, streaming, stats, directive parsing) */

#include "agent_framework.h"
#include "llm_client.h"
#include "config.h"
#include "os_api.h"
#include "agent_private.h"
#include "memfile.h"

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

    const char *endpoint = cfg && cfg->llm_endpoint[0]
                           ? cfg->llm_endpoint
                           : "http://localhost:8080/v1";

    const char *api_key = cfg && cfg->llm_api_key[0]
                          ? cfg->llm_api_key
                          : NULL;

    const char *model = cfg && cfg->llm_model[0]
                        ? cfg->llm_model
                        : "gpt-4";

    g_spinner_on = 1;
    g_ctx->first_token = 0;

    os_printf("\n");  /* keep prompt visible above, spinner below */
    fflush(stdout);

    os_thread_handle_t tid;
    os_thread_create(&tid, spinner_thread, NULL);

    /*
     * Build prompt:
     *   system instructions + memory context + user question
     */
    char prompt_buf[8192];
    const char *final_prompt = question;

    {
        int pos = 0;

        /*
         * System instructions:
         * tell LLM it can persist memory via markers.
         */
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
                for (int i = 0; i < g_ctx->mem.count; i++) {
                    pos += os_snprintf(prompt_buf + pos,
                                       sizeof(prompt_buf) - pos,
                                       "%s\n",
                                       g_ctx->mem.entries[i]);
                }
            }

            if (g_ctx->user.count > 0) {
                pos += os_snprintf(prompt_buf + pos,
                                   sizeof(prompt_buf) - pos,
                                   "===== PROFILE =====\n");

                for (int i = 0; i < g_ctx->user.count; i++) {
                    pos += os_snprintf(prompt_buf + pos,
                                       sizeof(prompt_buf) - pos,
                                       "%s\n",
                                       g_ctx->user.entries[i]);
                }
            }

            pos += os_snprintf(prompt_buf + pos,
                               sizeof(prompt_buf) - pos,
                               "\n");
        }

        /*
         * Flatten newlines to spaces to keep JSON valid.
         */
        for (int i = 0; i < pos; i++) {
            if (prompt_buf[i] == '\n')
                prompt_buf[i] = ' ';
        }

        /*
         * Append question and flatten its newlines too.
         */
        size_t qlen = os_strlen(question);

        for (size_t i = 0;
             i < qlen && pos < (int)sizeof(prompt_buf) - 1;
             i++) {
            char c = question[i];
            prompt_buf[pos++] = (c == '\n') ? ' ' : c;
        }

        prompt_buf[pos] = '\0';
        final_prompt = prompt_buf;
    }

    uint64_t t0 = os_clock_ms();

    llm_response_t *resp = llm_chat_stream(endpoint,
                                           api_key,
                                           model,
                                           final_prompt,
                                           on_llm_token,
                                           NULL);

    uint64_t elapsed = os_clock_ms() - t0;

    g_spinner_on = 0;
    os_thread_join(tid);

    if (!resp) {
        if (out)
            os_snprintf(out, out_len, RED "%% LLM unavailable" RST);

        /*
         * Close the answer box if it was opened.
         */
        if (g_ctx->saw_reasoning >= 2)
            print_answer_bottom();

        return -1;
    }

    resp->latency_ms = elapsed;

    /*
     * Close answer box.
     */
    if (g_ctx->saw_reasoning >= 2)
        print_answer_bottom();

    /*
     * If only reasoning box was opened but no content arrived,
     * close it too.
     */
    if (g_ctx->saw_reasoning == 1) {
        print_reasoning_bottom();
        os_printf("\n");
    }

    /*
     * Stats bar.
     */
    if (out) {
        size_t pos = 0;

        pos += os_snprintf(out + pos,
                           out_len - pos,
                           DIM "  %.1fs",
                           (double)elapsed / 1000.0);

        if (resp->completion_tokens > 0) {
            int total_tok = resp->completion_tokens
                          + (resp->prompt_tokens > 0
                             ? resp->prompt_tokens
                             : 0);

            pos += os_snprintf(out + pos,
                               out_len - pos,
                               " · %d tok",
                               total_tok);

            if (elapsed > 0) {
                double tps = (double)total_tok /
                             ((double)elapsed / 1000.0);

                pos += os_snprintf(out + pos,
                                   out_len - pos,
                                   " · %.1f/s",
                                   tps);
            }
        }

        /*
         * Track session tokens.
         */
        int sess_add = resp->prompt_tokens + resp->completion_tokens;

        if (sess_add > 0)
            g_ctx->session_tokens += sess_add;

        /*
         * Show memory usage if entries exist.
         */
        if (g_ctx->mem.count > 0 || g_ctx->user.count > 0) {
            char mu[48];
            char uu[48];

            if (g_ctx->mem.count > 0)
                memfile_usage(&g_ctx->mem, mu, sizeof(mu));

            if (g_ctx->user.count > 0)
                memfile_usage(&g_ctx->user, uu, sizeof(uu));

            pos += os_snprintf(out + pos,
                               out_len - pos,
                               DIM " ‖ " RST);

            if (g_ctx->mem.count > 0) {
                pos += os_snprintf(out + pos,
                                   out_len - pos,
                                   DIM "mem %s" RST,
                                   mu);
            }

            if (g_ctx->user.count > 0) {
                pos += os_snprintf(out + pos,
                                   out_len - pos,
                                   DIM " · you %s" RST,
                                   uu);
            }
        }

        os_snprintf(out + pos, out_len - pos, RST);
    }

    /*
     * Auto-log Q&A.
     */
    if (resp->content) {
        os_file_handle_t lf = os_file_open("conversations.log", "a");

        if (lf) {
            char logline[4096];

            int n = os_snprintf(logline,
                                sizeof(logline),
                                "[%llu] Q: %s\nA: %s\n\n",
                                (unsigned long long)t0,
                                question,
                                resp->content);

            if (n > 0)
                os_file_write(lf, logline, (size_t)n);

            os_file_close(lf);
        }

        /*
         * Parse memory directives from LLM response.
         */
        const char *p = resp->content;

        while (p) {
            const char *note_start = strstr(p, "[NOTE: ");
            const char *prof_start = strstr(p, "[PROFILE: ");
            const char *forg_start = strstr(p, "[FORGET: ");

            const char *earliest = NULL;
            int kind = 0;       /* 1=note, 2=profile, 3=forget */
            int prefix_len = 0;

            if (note_start && (!earliest || note_start < earliest)) {
                earliest = note_start;
                kind = 1;
                prefix_len = 7;
            }

            if (prof_start && (!earliest || prof_start < earliest)) {
                earliest = prof_start;
                kind = 2;
                prefix_len = 10;
            }

            if (forg_start && (!earliest || forg_start < earliest)) {
                earliest = forg_start;
                kind = 3;
                prefix_len = 9;
            }

            if (!earliest)
                break;

            const char *start = earliest + prefix_len;
            const char *end = strstr(start, "]");

            if (!end)
                break;

            /*
             * Extract content between [MARKER: and ].
             */
            size_t clen = (size_t)(end - start);

            if (clen > 0) {
                char content[1024];
                size_t cp = clen < sizeof(content) - 1
                            ? clen
                            : sizeof(content) - 1;

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

                if (feedback[0])
                    os_printf(DIM "%s" RST "\n", feedback);
            }

            p = end + 1;
        }
    }

    llm_response_free(resp);
    return 0;
}
