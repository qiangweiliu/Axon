/*
 * agent_framework.h — 框架核心公共 API
 */

#ifndef AGENT_FRAMEWORK_H
#define AGENT_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* =========================================================================
 * 1. Module State
 * ========================================================================= */

typedef enum {
    FRAMEWORK_STATE_UNLOADED,
    FRAMEWORK_STATE_INITED,
    FRAMEWORK_STATE_STARTED,
    FRAMEWORK_STATE_RUNNING,
    FRAMEWORK_STATE_STOPPING,
    FRAMEWORK_STATE_DEINITED
} framework_state_t;

/* =========================================================================
 * 2. Framework Module Base Struct
 * ========================================================================= */

typedef struct framework_module {
    const char *name;
    unsigned int version;
    int priority;
    volatile framework_state_t state;

    int  (*init)(struct framework_module *mod);
    int  (*start)(struct framework_module *mod);
    void (*loop)(struct framework_module *mod);
    int  (*stop)(struct framework_module *mod);
    int  (*deinit)(struct framework_module *mod);

    unsigned int layer;   /* LAYER_APP/BUSINESS/INFRA/CORE */
    unsigned int offset;  /* sub-priority within layer (0..99) */
    void *ctx;
    unsigned int id;
    struct framework_module *next;
} framework_module_t;

/* =========================================================================
 * 3. Module Registration — ELF Section-Based
 *
 * Modules place a pointer to their framework_module_t in the .agent_modules
 * ELF section. framework_discover_modules() scans the section at init time.
 * Priority is auto-computed from layer + offset — no manual .priority needed.
 *
 * Layer → base priority (higher = earlier init):
 *   LAYER_CORE     (3) →  900  (logger, config)
 *   LAYER_INFRA    (2) →  700  (threadpool, http_client)
 *   LAYER_BUSINESS (1) →  400  (memory, tool_manager, llm_client)
 *   LAYER_APP      (0) →  100  (agent_loop)
 *
 * Offset: order within the layer (0..99).
 * Priority = layer * 300 + 100 + offset.
 * ========================================================================= */

void _fw_module_register(framework_module_t *mod);

#define LAYER_APP      0
#define LAYER_BUSINESS 1
#define LAYER_INFRA    2
#define LAYER_CORE     3

/* Generic: place a typed pointer into a named ELF section. */
#define AGENT_SECTION(section_name, type, var) \
    __attribute__((section("." section_name), used)) \
    type *_ag_sec_##var = &var

/* Register a module. Priority is auto-computed from struct's
   .layer and .offset fields (set in the struct initializer).
   The constructor runs before main, set priority = layer*300+100+offset. */
#define MODULE_REGISTER(mod) \
    AGENT_SECTION("agent_modules", framework_module_t, mod); \
    __attribute__((constructor(65535))) \
    static void _ag_set_prio_##mod(void) { \
        mod.priority = (mod).layer * 300 + 100 + (mod).offset; \
    }

/* =========================================================================
 * 4. Lifecycle API
 * ========================================================================= */

int  framework_init(void);
void framework_shutdown(void);
void framework_loop_tick(void);

/* =========================================================================
 * 5. Event Bus API
 * ========================================================================= */

typedef uint32_t framework_event_type_t;

/* Pre-defined event types */
#define FW_EVENT_CONFIG_LOADED   ((framework_event_type_t)1)
#define FW_EVENT_START_DONE      ((framework_event_type_t)2)
#define FW_EVENT_SHUTDOWN        ((framework_event_type_t)3)

typedef void (*framework_event_handler_t)(framework_event_type_t type,
                                          const void *data,
                                          size_t data_size,
                                          void *user_data);

int framework_event_subscribe(framework_event_type_t type,
                              framework_event_handler_t handler,
                              int priority,
                              void *user_data);

int framework_event_unsubscribe(framework_event_type_t type,
                                framework_event_handler_t handler);

int framework_event_publish(framework_event_type_t type,
                            const void *data,
                            size_t data_size);

/* Must be called before any pub/sub. */
int framework_bus_init(void);

/* =========================================================================
 * 6. Logger API
 * ========================================================================= */

#include "logger.h"

#define LOG_DEBUG(fmt, ...)   _fw_log(FW_LOG_DEBUG,   fw_log_get_module(), fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    _fw_log(FW_LOG_INFO,    fw_log_get_module(), fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    _fw_log(FW_LOG_WARN,    fw_log_get_module(), fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   _fw_log(FW_LOG_ERROR,   fw_log_get_module(), fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)   _fw_log(FW_LOG_FATAL,   fw_log_get_module(), fmt, ##__VA_ARGS__)

/* =========================================================================
 * 7. Memory Allocator API
 * ========================================================================= */

void *framework_alloc(size_t size);
void  framework_free(void *ptr);
void  framework_leak_report(void);

/* =========================================================================
 * 8. Monitor API
 * ========================================================================= */

void framework_monitor_print(void);

/* =========================================================================
 * 9. Module Lookup
 * ========================================================================= */

framework_module_t *framework_get_module_by_name(const char *name);

#endif /* AGENT_FRAMEWORK_H */
