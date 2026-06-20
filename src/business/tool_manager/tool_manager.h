/*
 * tool_manager.h — Tool registration and execution API
 *
 * Business layer (priority=380). Tools are named callbacks with
 * JSON parameter schemas. Thread-safe registry.
 */

#ifndef BUSINESS_TOOL_MANAGER_H
#define BUSINESS_TOOL_MANAGER_H

#include <stddef.h>

/* Risk levels for tool permission policy */
#define TOOL_RISK_SAFE       0  /* read-only, always allowed */
#define TOOL_RISK_WRITE      1  /* modifies files/state */
#define TOOL_RISK_SHELL      2  /* runs shell commands */
#define TOOL_RISK_DANGEROUS  3  /* destructive, needs explicit confirm */

/* Maximum result buffer for tool_call() */
#define TOOL_RESULT_MAX  8192

/* Tool definition — used when registering */
typedef struct {
    const char *name;           /* unique tool name */
    const char *description;    /* human-readable description */
    const char *params_json;    /* JSON Schema for parameters */
    int         risk;           /* TOOL_RISK_* */
    int (*execute)(const char *args_json,   /* JSON arguments */
                   char *result,            /* output buffer */
                   size_t result_len,       /* output buffer size */
                   void *user_data);        /* opaque context */
} tool_def_t;

/* Tool info — read-only query result (no execute callback) */
typedef struct {
    const char *name;
    const char *description;
    const char *params_json;
    int         risk;
    int         enabled;
} tool_info_t;

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

/* Number of registered tools */
int tool_count(void);

/*
 * Get tool info by index. Returns 0 on success, -1 if index out of range.
 */
int tool_get_info(int index, tool_info_t *info);

/*
 * Find tool by name. Returns 0 on success, -1 if not found.
 */
int tool_find(const char *name, tool_info_t *info);

/*
 * Validate tool arguments against the registered params_json schema.
 * Lightweight checks: required fields present, non-empty strings.
 * Returns 0 on valid, -1 on invalid (error message in err buffer).
 */
int tool_validate(const char *name, const char *args_json,
                  char *err, size_t err_len);

#endif /* BUSINESS_TOOL_MANAGER_H */
