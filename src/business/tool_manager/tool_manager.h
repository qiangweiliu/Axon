/*
 * tool_manager.h — Tool registration and execution API
 *
 * Business layer (priority=380). Tools are named callbacks with
 * JSON parameter schemas. Thread-safe registry.
 */

#ifndef BUSINESS_TOOL_MANAGER_H
#define BUSINESS_TOOL_MANAGER_H

#include <stddef.h>

/* Maximum result buffer for tool_call() */
#define TOOL_RESULT_MAX  4096

/* Tool definition */
typedef struct {
    const char *name;           /* unique tool name */
    const char *description;    /* human-readable description */
    const char *params_json;    /* JSON Schema for parameters */
    int (*execute)(const char *args_json,   /* JSON arguments */
                   char *result,            /* output buffer */
                   size_t result_len,       /* output buffer size */
                   void *user_data);        /* opaque context */
} tool_def_t;

/*
 * Register a tool. Returns 0 on success, -1 if name exists or full.
 * Thread-safe.
 */
int tool_register(const tool_def_t *tool);

/*
 * Call a registered tool by name.
 * args_json: JSON string of arguments (or "{}" if none)
 * result: output buffer (caller-allocated, TOOL_RESULT_MAX recommended)
 * result_len: size of result buffer
 * Returns: tool's execute() return value, or -1 if tool not found.
 */
int tool_call(const char *name, const char *args_json,
              char *result, size_t result_len);

/*
 * List all registered tools as a JSON array.
 * Format: [{"name":"x","description":"...","params":{...}}, ...]
 * Returns number of tools, or -1 on buffer overflow.
 */
int tool_list_json(char *buf, size_t buf_len);

/* Number of registered tools */
int tool_count(void);

#endif /* BUSINESS_TOOL_MANAGER_H */
