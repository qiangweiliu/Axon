/*
 * llm_client.h — LLM API client (OpenAI-compatible)
 *
 * Business layer (priority=350). Wraps http_client for chat completion.
 * Extracts content + usage stats from API response JSON.
 */

#ifndef BUSINESS_LLM_CLIENT_H
#define BUSINESS_LLM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char   *content;
    size_t  content_len;
    int     prompt_tokens;
    int     completion_tokens;
    uint64_t latency_ms;   /* set by caller, not the API */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/*
 * Send a chat completion request to an OpenAI-compatible API.
 * endpoint: full URL base, e.g. "https://apihub.agnes-ai.com/v1"
 * api_key:  API key (can be NULL for local servers)
 * model:    model name, e.g. "agnes-2.0-flash"
 * prompt:   user message
 *
 * Returns malloc'd llm_response_t, or NULL on failure.
 * The response includes usage.prompt_tokens + usage.completion_tokens
 * when the API returns them in the JSON.
 * latency_ms must be set by caller after timing.
 * Caller must llm_response_free().
 */
llm_response_t *llm_chat(const char *endpoint,
                         const char *api_key,
                         const char *model,
                         const char *prompt);

/*
 * Streaming chat completion — calls on_token for each received token.
 * Returns the same response_t as llm_chat (full accumulated content).
 * Set response->latency_ms yourself after timing.
 */
typedef void (*llm_token_cb_t)(const char *token, size_t len,
                               int tokens_so_far, uint64_t elapsed_ms,
                               void *user);
llm_response_t *llm_chat_stream(const char *endpoint,
                                const char *api_key,
                                const char *model,
                                const char *prompt,
                                llm_token_cb_t on_token, void *user);

#endif /* BUSINESS_LLM_CLIENT_H */
