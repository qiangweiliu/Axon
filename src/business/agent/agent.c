/*
 * agent.c — Agent core loop
 *
 * Pure tool loop: build prompt → call LLM → parse tool → execute → repeat.
 * No knowledge of skills, memory, language, or topics.
 */

#include "agent.h"
#include "tool_schema.h"
#include "tool_executor.h"
#include "llm_client.h"
#include "os_api.h"
#include <string.h>

/* ── Debug dump helpers ───────────────────────────────────────────── */

#define DIM "\033[2m"
#define RST "\033[0m"

static void debug_dump(const char *label, int round, const char *data, size_t len, int is_prompt)
{
    if (is_prompt)
        os_fprintf_stderr(DIM "──[AGENT R%d PROMPT] (%zu bytes)───────────" RST "\n",
                          round, len);
    else
        os_fprintf_stderr(DIM "──[AGENT R%d RESPONSE] (%zu bytes)─────────" RST "\n",
                          round, len);
    os_fprintf_stderr("%s\n", data ? data : "(null)");
    os_fprintf_stderr(DIM "──────────────────────────────────────────" RST "\n");
    (void)label;
}

int agent_run(const char *endpoint,
              const char *api_key,
              const char *model,
              const agent_context_t *ctx,
              const char *question,
              char *answer,
              size_t answer_len)
{
    if (!endpoint || !model || !ctx || !question || !answer || answer_len < 2)
        return -1;
    (void)question;  /* question is embedded in base_prompt, not used separately */

    char buf[128 * 1024];
    int depth = 0;
    int max_depth = ctx->max_depth > 0 ? ctx->max_depth : AGENT_MAX_DEPTH;

    /* Build initial prompt from base_prompt (ask.c's BUILD_PROMPT already
     * includes system, tool schema, skills, language, topics, and question).
     * Tool schema and question are NOT re-added — the base already has them. */
    int pos = 0;
    if (ctx->base_prompt)
        pos += os_snprintf(buf + pos, sizeof(buf) - pos, "%s", ctx->base_prompt);

    /* Save the base length so we can truncate back on retries */
    int base_end = pos;

    /* Buffer for preserving LLM's last tool-call response across iterations */
    char prev_llm_output[4096];
    prev_llm_output[0] = '\0';

    while (depth < max_depth) {
        if (ctx->debug)
            debug_dump("prompt", depth + 1, buf, os_strlen(buf), 1);

        llm_response_t *resp = llm_chat(endpoint, api_key, model, buf);
        if (!resp) {
            os_snprintf(answer, answer_len, "Error: LLM unavailable");
            return -1;
        }
        if (!resp->content) {
            llm_response_free(resp);
            os_snprintf(answer, answer_len, "Error: empty LLM response");
            return -1;
        }

        if (ctx->debug)
            debug_dump("response", depth + 1, resp->content, resp->content_len, 0);

        tool_call_t call;
        tool_parse_status_t status = tool_parse_call(resp->content, &call);

        if (status == TOOL_PARSE_NONE) {
            os_strncpy(answer, resp->content, answer_len);
            llm_response_free(resp);
            return 0;
        }

        char tool_result[8192];

        if (status == TOOL_PARSE_INVALID) {
            os_snprintf(tool_result, sizeof(tool_result),
                "Error: tool call format is invalid.\n"
                "Use <tool_call>...</tool_call> with proper JSON.\n");
        } else {
            tool_execute_call(&call, tool_result, sizeof(tool_result));
        }

        /* Save LLM's output for the next iteration's context */
        size_t prev_len = os_strlen(resp->content);
        if (prev_len >= sizeof(prev_llm_output))
            prev_len = sizeof(prev_llm_output) - 1;
        os_memcpy(prev_llm_output, resp->content, prev_len);
        prev_llm_output[prev_len] = '\0';

        /* Truncate back to base (which already includes the question),
         * then append tool call history for context */
        pos = base_end;
        /* Include the LLM's own previous tool call so it sees the chain */
        if (prev_llm_output[0]) {
            pos += os_snprintf(buf + pos, sizeof(buf) - pos,
                "Previous assistant output:\n%s\n", prev_llm_output);
        }
        pos += os_snprintf(buf + pos, sizeof(buf) - pos,
            "Tool result:\n%s\n", tool_result);

        llm_response_free(resp);
        depth++;
    }

    os_snprintf(answer, answer_len,
        "[Completed after %d tool call(s)]\n%s",
        depth, prev_llm_output);
    return 1;
}
