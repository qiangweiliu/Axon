/*
 * agent.h — Agent core loop (tool loop, NOT ask.c)
 *
 * Business layer. Separates 'build context' (ask.c) from
 * 'tool loop' (agent.c). agent_run() takes an already-built
 * base_prompt and runs the while(depth<max) loop.
 */

#ifndef BUSINESS_AGENT_H
#define BUSINESS_AGENT_H

#include <stddef.h>

#define AGENT_MAX_DEPTH    6
#define AGENT_MAX_RESPONSE 8192

typedef struct {
    const char *base_prompt;    /* system + interface + skills + language + topics + memory */
    int         max_depth;      /* max tool call iterations (default 6) */
    int         debug;          /* if true, dump full prompts and responses to stderr */
} agent_context_t;

/*
 * Main agent loop.
 *
 * endpoint/api_key/model: LLM connection params (same as llm_chat)
 * ctx:      agent context (base_prompt + max_depth)
 * question: user's question
 * answer:   output buffer for final response
 * answer_len: size of answer buffer
 *
 * Returns 0 on success, 1 if max depth reached, -1 on error.
 */
int agent_run(const char *endpoint,
              const char *api_key,
              const char *model,
              const agent_context_t *ctx,
              const char *question,
              char *answer,
              size_t answer_len);

#endif /* BUSINESS_AGENT_H */
