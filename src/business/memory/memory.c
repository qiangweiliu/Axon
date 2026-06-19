/*
 * memory.c — Memory management core module
 *
 * Dispatches all memory_* calls to the active backend.
 * Default backend: memory_backend_file (backend_file.c).
 *
 * Swap backends: implement memory_backend_t, call memory_set_backend().
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "memory.h"

/* Default backend (defined in backend_file.c) */
extern const memory_backend_t memory_backend_file;

static const memory_backend_t *g_backend = NULL;

/* ── Backend Selection ────────────────────────────────────────────── */

void memory_set_backend(const memory_backend_t *backend)
{
    if (backend) {
        g_backend = backend;
        LOG_INFO("Memory: backend replaced");
    }
}

static int ensure_backend(void)
{
    if (g_backend) return 0;
    g_backend = &memory_backend_file;
    return g_backend->init();
}

/* ── Public API ───────────────────────────────────────────────────── */

int memory_store(const memory_entry_t *entry, char *id_out, size_t id_len)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->store(entry, id_out, id_len);
}

int memory_retrieve(const char *id, memory_entry_t *entry_out)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->retrieve(id, entry_out);
}

int memory_update(const memory_entry_t *entry)
{
    if (ensure_backend() != 0 || !entry) return -1;
    return g_backend->update(entry->id, entry);
}

int memory_remove(const char *id)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->remove(id);
}

int memory_search(const char *query, memory_entry_t *results,
                  int max_results, int *out_count)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->search(query, results, max_results, out_count);
}

int memory_count(void)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->count();
}

int memory_clear(void)
{
    if (ensure_backend() != 0) return -1;
    return g_backend->clear();
}

/* ── Module Registration ──────────────────────────────────────────── */

static int memory_init(framework_module_t *mod)
{
    (void)mod;

    g_backend = &memory_backend_file;
    if (g_backend->init() != 0) {
        LOG_ERROR("Memory: backend init failed");
        return -1;
    }

    LOG_INFO("Memory: init (file backend, %d entries)", g_backend->count());
    return 0;
}

static int memory_start(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("Memory: ready");
    return 0;
}

static int memory_stop(framework_module_t *mod)
{
    (void)mod;
    if (g_backend) g_backend->shutdown();
    LOG_INFO("Memory: stop");
    return 0;
}

    framework_module_t memory_mod = {
    .name     = "memory",
    .version  = 0x00010000,
    
    .state    = FRAMEWORK_STATE_UNLOADED,
    .init     = memory_init,
    .start    = memory_start,
    .loop     = NULL,
    .stop     = memory_stop,
    .deinit   = NULL,
    .ctx      = NULL,
    .id       = 0,
    .next     = NULL,
    .priority = PRIORITY_BUSINESS + 0,
};

MODULE_REGISTER(memory_mod);
