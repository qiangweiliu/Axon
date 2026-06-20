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

    char buf[128 * 1024];
    int depth = 0;
    int max_depth = ctx->max_depth > 0 ? ctx->max_depth : AGENT_MAX_DEPTH;

    /* Round 0: build initial prompt */
    int pos = 0;
    if (ctx->base_prompt)
        pos += os_snprintf(buf + pos, sizeof(buf) - pos, "%s", ctx->base_prompt);

    /* Save the base length so we can truncate back on retries */
    int base_end = pos;

    /* Append tool schema */
    pos += tool_schema_build(buf + pos, sizeof(buf) - pos);

    /* Append user question (only added once — it's always the same) */
    pos += os_snprintf(buf + pos, sizeof(buf) - pos, "User: %s\n", question);

    while (depth < max_depth) {
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

        /* Truncate back to base+question, append tool result */
        pos = base_end;
        pos += os_snprintf(buf + pos, sizeof(buf) - pos, "User: %s\n", question);
        pos += os_snprintf(buf + pos, sizeof(buf) - pos,
            "Tool result:\n%s\n", tool_result);

        llm_response_free(resp);
        depth++;
    }

    os_snprintf(answer, answer_len,
        "Max depth (%d) reached. The task may be incomplete.", max_depth);
    return 1;
}
