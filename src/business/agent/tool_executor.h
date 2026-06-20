/*
 * tool_executor.h — Parse, validate, execute, and format tool calls
 *
 * Three-state parsing:
 *   NONE    → no tool call found (final answer)
 *   OK      → tool call parsed and executed
 *   INVALID → malformed tool call (LLM should retry)
 */

#ifndef BUSINESS_TOOL_EXECUTOR_H
#define BUSINESS_TOOL_EXECUTOR_H

#include <stddef.h>

typedef enum {
    TOOL_PARSE_NONE = 0,    /* no <tool_call> in response → final answer */
    TOOL_PARSE_OK,          /* tool call parsed & executed */
    TOOL_PARSE_INVALID,     /* malformed tool call → send error back to LLM */
} tool_parse_status_t;

typedef struct {
    char name[64];
    char args_json[2048];
} tool_call_t;

/*
 * Parse a <tool_call>...</tool_call> block from LLM response.
 *
 * LLM format:
 *   <tool_call>
 *   {"name":"list_dir","arguments":{"path":"."}}
 *   </tool_call>
 *
 * response: LLM text output
 * call:     parsed tool call (on TOOL_PARSE_OK)
 *
 * Returns parse status.
 */
tool_parse_status_t tool_parse_call(const char *response, tool_call_t *call);

/*
 * Execute a tool call and format the result for LLM consumption.
 *
 * On success, result format:
 *   <tool_result>
 *   tool: list_dir
 *   status: success
 *   content:
 *   {"entries":["a.txt","b.txt"]}
 *   </tool_result>
 *
 * On failure:
 *   <tool_result>
 *   tool: list_dir
 *   status: error
 *   message: cannot open directory
 *   </tool_result>
 *
 * Returns 0 on success (tool executed), -1 on parse/validation error.
 */
int tool_execute_call(const tool_call_t *call, char *result, size_t result_len);

#endif /* BUSINESS_TOOL_EXECUTOR_H */
