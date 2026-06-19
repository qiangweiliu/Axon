/*
 * lifecycle.c — Module lifecycle management with rollback on failure.
 *
 * Supports both sequential execution (fallback) and threadpool-accelerated
 * parallel execution for init/start phases. When g_threadpool is available
 * (set by the threadpool module's init callback), remaining modules are
 * submitted to the pool for parallel execution.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "threadpool.h"

static void set_state(framework_module_t *mod, framework_state_t state)
{
    mod->state = state;
}

/* =========================================================================
 * Parallel execution wrappers
 *
 * These adapt framework_module_t callbacks to threadpool_task_fn_t.
 * NOTE: fw_log_bind_module() uses a global pointer — log module names
 *       may be interleaved during parallel execution. This is cosmetic.
 * ========================================================================= */

static int _parallel_init_fn(void *arg)
{
    framework_module_t *mod = (framework_module_t *)arg;
    fw_log_bind_module(mod->name);
    return mod->init ? mod->init(mod) : 0;
}

static int _parallel_start_fn(void *arg)
{
    framework_module_t *mod = (framework_module_t *)arg;
    fw_log_bind_module(mod->name);
    return mod->start ? mod->start(mod) : 0;
}

/* =========================================================================
 * Init phase — sequential up to threadpool, then parallel
 * ========================================================================= */

int call_init_all(void)
{
    int i;

    /* Phase 1: sequential init until threadpool module activates the pool */
    for (i = 0; i < g_registry.count; i++) {
        framework_module_t *mod = g_registry.list[i];
        fw_log_bind_module(mod->name);

        int rc = mod->init ? mod->init(mod) : 0;
        if (rc != 0) {
            LOG_ERROR("Framework: module '%s' init failed (rc=%d), rolling back",
                      mod->name, rc);
            for (int j = i - 1; j >= 0; j--) {
                framework_module_t *prev = g_registry.list[j];
                if (prev->state == FRAMEWORK_STATE_INITED && prev->deinit) {
                    LOG_WARN("  Rolling back: deinit '%s'", prev->name);
                    prev->deinit(prev);
                    set_state(prev, FRAMEWORK_STATE_DEINITED);
                }
            }
            return -1;
        }
        set_state(mod, FRAMEWORK_STATE_INITED);

        /* Threadpool module just completed init — switch to parallel */
        if (g_threadpool != NULL) {
            i++;  /* move past threadpool */
            break;
        }
    }

    /* Phase 2: if threadpool available and modules remain, parallel init */
    if (g_threadpool != NULL && i < g_registry.count) {
        int remaining = g_registry.count - i;
        int *results = (int *)os_calloc((size_t)remaining, sizeof(int));
        if (!results) {
            LOG_ERROR("%s", "Framework: out of memory for parallel init results");
            return -1;
        }

        /* Submit all remaining modules to threadpool */
        for (int j = i; j < g_registry.count; j++) {
            results[j - i] = 0;
            int rc = threadpool_submit(g_threadpool, _parallel_init_fn,
                                       g_registry.list[j], &results[j - i]);
            if (rc != 0) {
                LOG_ERROR("Framework: threadpool_submit failed for '%s'",
                          g_registry.list[j]->name);
                os_free(results);
                return -1;
            }
        }

        threadpool_wait(g_threadpool);

        /* Check results and set state — if any failed, rollback everything */
        for (int j = i; j < g_registry.count; j++) {
            framework_module_t *mod = g_registry.list[j];
            int rc = results[j - i];
            if (rc != 0) {
                LOG_ERROR("Framework: module '%s' init failed (rc=%d), rolling back",
                          mod->name, rc);
                /* Rollback all previously inited modules (sequential + parallel) */
                for (int k = j - 1; k >= 0; k--) {
                    framework_module_t *prev = g_registry.list[k];
                    if (prev->state == FRAMEWORK_STATE_INITED && prev->deinit) {
                        LOG_WARN("  Rolling back: deinit '%s'", prev->name);
                        prev->deinit(prev);
                        set_state(prev, FRAMEWORK_STATE_DEINITED);
                    }
                }
                os_free(results);
                return -1;
            }
            set_state(mod, FRAMEWORK_STATE_INITED);
        }

        os_free(results);
    }

    return 0;
}

/* =========================================================================
 * Start phase — sequential up to threadpool start, then parallel
 * ========================================================================= */

int call_start_all(void)
{
    int i;

    /* Phase 1: sequential start until threadpool module completes start */
    for (i = 0; i < g_registry.count; i++) {
        framework_module_t *mod = g_registry.list[i];
        fw_log_bind_module(mod->name);

        int rc = mod->start ? mod->start(mod) : 0;
        if (rc != 0) {
            LOG_ERROR("Framework: module '%s' start failed (rc=%d), rolling back",
                      mod->name, rc);
            for (int j = i - 1; j >= 0; j--) {
                framework_module_t *prev = g_registry.list[j];
                if (prev->state == FRAMEWORK_STATE_STARTED && prev->stop) {
                    LOG_WARN("  Rolling back: stop '%s'", prev->name);
                    prev->stop(prev);
                    set_state(prev, FRAMEWORK_STATE_INITED);
                }
            }
            return -1;
        }
        set_state(mod, FRAMEWORK_STATE_STARTED);

        /* Threadpool module just completed start — switch to parallel */
        if (g_threadpool != NULL) {
            i++;
            break;
        }
    }

    /* Phase 2: if threadpool available and modules remain, parallel start */
    if (g_threadpool != NULL && i < g_registry.count) {
        int remaining = g_registry.count - i;
        int *results = (int *)os_calloc((size_t)remaining, sizeof(int));
        if (!results) {
            LOG_ERROR("%s", "Framework: out of memory for parallel start results");
            return -1;
        }

        for (int j = i; j < g_registry.count; j++) {
            results[j - i] = 0;
            int rc = threadpool_submit(g_threadpool, _parallel_start_fn,
                                       g_registry.list[j], &results[j - i]);
            if (rc != 0) {
                LOG_ERROR("Framework: threadpool_submit failed for '%s'",
                          g_registry.list[j]->name);
                os_free(results);
                return -1;
            }
        }

        threadpool_wait(g_threadpool);

        for (int j = i; j < g_registry.count; j++) {
            framework_module_t *mod = g_registry.list[j];
            int rc = results[j - i];
            if (rc != 0) {
                LOG_ERROR("Framework: module '%s' start failed (rc=%d), rolling back",
                          mod->name, rc);
                for (int k = j - 1; k >= 0; k--) {
                    framework_module_t *prev = g_registry.list[k];
                    if (prev->state == FRAMEWORK_STATE_STARTED && prev->stop) {
                        LOG_WARN("  Rolling back: stop '%s'", prev->name);
                        prev->stop(prev);
                        set_state(prev, FRAMEWORK_STATE_INITED);
                    }
                }
                os_free(results);
                return -1;
            }
            set_state(mod, FRAMEWORK_STATE_STARTED);
        }

        os_free(results);
    }

    /* All modules started — transition to RUNNING */
    for (int i = 0; i < g_registry.count; i++) {
        g_registry.list[i]->state = FRAMEWORK_STATE_RUNNING;
    }

    LOG_INFO("Framework: all modules started successfully");

    /* Publish start-done event — modules can react */
    framework_event_publish(FW_EVENT_START_DONE, &g_registry.count,
                            sizeof(g_registry.count));

    return 0;
}

/* =========================================================================
 * Stop phase — always sequential (reverse order)
 * ========================================================================= */

void call_stop_all(void)
{
    for (int i = g_registry.count - 1; i >= 0; i--) {
        framework_module_t *mod = g_registry.list[i];
        if (mod->state == FRAMEWORK_STATE_RUNNING && mod->stop) {
            set_state(mod, FRAMEWORK_STATE_STOPPING);
            mod->stop(mod);
        }
    }
}

/* =========================================================================
 * Deinit phase — always sequential (reverse order)
 * ========================================================================= */

void call_deinit_all(void)
{
    for (int i = g_registry.count - 1; i >= 0; i--) {
        framework_module_t *mod = g_registry.list[i];
        if ((mod->state == FRAMEWORK_STATE_INITED ||
             mod->state == FRAMEWORK_STATE_STARTED ||
             mod->state == FRAMEWORK_STATE_STOPPING ||
             mod->state == FRAMEWORK_STATE_RUNNING) && mod->deinit) {
            mod->deinit(mod);
        }
        set_state(mod, FRAMEWORK_STATE_DEINITED);
    }
}

/* =========================================================================
 * Loop tick
 * ========================================================================= */

void framework_loop_tick(void)
{
    for (int i = 0; i < g_registry.count; i++) {
        framework_module_t *mod = g_registry.list[i];
        if (mod->state == FRAMEWORK_STATE_RUNNING && mod->loop) {
            fw_log_bind_module(mod->name);
            mod->loop(mod);
        }
    }
}
