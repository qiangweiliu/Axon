/*
 * monitor.c — Runtime state printer.
 *
 * framework_monitor_print() dumps every registered module's
 * name, state, priority and id to the log.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"

static const char *state_str(framework_state_t s)
{
    switch (s) {
    case FRAMEWORK_STATE_UNLOADED: return "UNLOADED";
    case FRAMEWORK_STATE_INITED:   return "INITED";
    case FRAMEWORK_STATE_STARTED:  return "STARTED";
    case FRAMEWORK_STATE_RUNNING:  return "RUNNING";
    case FRAMEWORK_STATE_STOPPING: return "STOPPING";
    case FRAMEWORK_STATE_DEINITED: return "DEINITED";
    default:                       return "?";
    }
}

void framework_monitor_print(void)
{
    int n = g_registry.count;

    fw_log_bind_module("monitor");

    LOG_INFO("========================================");
    LOG_INFO("  FRAMEWORK MONITOR  (%d module%s)", n, n == 1 ? "" : "s");
    LOG_INFO("========================================");

    for (int i = 0; i < n; i++) {
        framework_module_t *mod = g_registry.list[i];
        LOG_INFO("  [%2d] %-16s  pri=%-4d  state=%-10s  id=%u",
                 i, mod->name ? mod->name : "(null)",
                 mod->priority, state_str(mod->state), mod->id);
    }

    LOG_INFO("========================================");
}
