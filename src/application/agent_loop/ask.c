/* ask.c — LLM interaction (spinner, streaming, stats, directive parsing) */

#include "agent_framework.h"
#include "llm_client.h"
#include "config.h"
#include "os_api.h"
#include "agent_private.h"
#include "memfile.h"
#include <string.h>
#include <stdio.h>


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

int handle_ask(const char *question, char *out, size_t out_len)
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
