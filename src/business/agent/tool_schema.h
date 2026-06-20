/*
 * tool_schema.h — Auto-generate tool descriptions from tool_manager
 *
 * Builds the "Available tools" section of the prompt by iterating
 * the tool_manager registry. No hardcoded tool names.
 */

#ifndef BUSINESS_TOOL_SCHEMA_H
#define BUSINESS_TOOL_SCHEMA_H

#include <stddef.h>

/*
 * Build tool description text into buf.
 * Format:
 *
 *   Tool: list_dir
 *     Description: List directory contents
 *     Args: {"path": "<dir>"}
 *
 * Returns bytes written.
 */
int tool_schema_build(char *buf, size_t len);

#endif /* BUSINESS_TOOL_SCHEMA_H */
