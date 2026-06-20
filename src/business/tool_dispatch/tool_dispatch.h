/*
 * tool_dispatch.h — Parse and execute [TOOL:] directives
 *
 * Business layer (priority=385). Isolates tool parsing + execution
 * from the application layer. Application calls one function;
 * this module handles name/args parsing and tool_manager dispatch.
 */

#ifndef BUSINESS_TOOL_DISPATCH_H
#define BUSINESS_TOOL_DISPATCH_H

#include <stddef.h>

/*
 * Parse a [TOOL:] directive text and execute the tool.
 *
 * text: content between "[TOOL:" and "]" (e.g. "list_dir | args={\"path\":\".\"}")
 * result: output buffer for the formatted result
 * result_len: size of result buffer
 *
 * Returns 0 on success, -1 on failure (tool not found or parse error).
 * On success, result contains the tool output (JSON string).
 * On failure, result contains an error message.
 */
int tool_dispatch(const char *text, char *result, size_t result_len);

#endif /* BUSINESS_TOOL_DISPATCH_H */
