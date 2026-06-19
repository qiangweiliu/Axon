/*
 * logger.h — Logger public API (placed in include/ for cross-layer access).
 */

#ifndef AGENT_LOGGER_H
#define AGENT_LOGGER_H

#include <stdarg.h>
#include <stdint.h>

typedef enum {
    FW_LOG_DEBUG,
    FW_LOG_INFO,
    FW_LOG_WARN,
    FW_LOG_ERROR,
    FW_LOG_FATAL
} fw_log_level_t;

typedef enum {
    FW_LOG_BACKEND_NONE,
    FW_LOG_BACKEND_BUFFERED
} fw_log_backend_t;

int  fw_log_init(const char *log_file, fw_log_level_t global_level);
int  fw_log_set_level(const char *module_name, fw_log_level_t level);
void fw_log_shutdown(void);
void fw_log_switch(fw_log_backend_t backend);
fw_log_backend_t fw_log_get_backend(void);
void fw_log_bind_module(const char *module_name);
const char *fw_log_get_module(void);
void _fw_log(fw_log_level_t level, const char *module_name, const char *fmt, ...);

#endif
