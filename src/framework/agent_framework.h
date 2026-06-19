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

    void *ctx;
    unsigned int id;
    uint8_t reserved[12];
    struct framework_module *next;
} framework_module_t;

/* =========================================================================
 * 3. Module Registration — ELF Section-Based
 *
 * Modules place a pointer to their framework_module_t in the .agent_modules
 * ELF section. framework_discover_modules() scans the section at init time.
 * This replaces __attribute__((constructor)), giving the framework full
 * control over module discovery order and timing.
 * ========================================================================= */

void _fw_module_register(framework_module_t *mod);

/* Place a pointer in the .agent_modules section.
   The pointer will be discovered by the section scanner at init. */
#define AGENT_MOD_SECTION \
    __attribute__((section(".agent_modules"), used))

/* Register a module: places &mod_instance in the .agent_modules section.
   The module struct MUST be non-static (global or file-scope without static).
   It will be auto-discovered by framework_discover_modules(). */
#define MODULE_REGISTER(mod_instance) \
    AGENT_MOD_SECTION \
    framework_module_t *_fw_mod_ptr_##mod_instance = &mod_instance

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
