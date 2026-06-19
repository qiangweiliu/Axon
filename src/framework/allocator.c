/*
 * allocator.c — Unified memory allocator with leak tracking.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"

static alloc_tracker_t g_alloc_tracker;
static os_mutex_handle_t g_alloc_mutex = NULL;

void *framework_alloc(size_t size)
{
    void *ptr = os_alloc(size);
    if (!ptr) return NULL;

    os_mutex_lock(g_alloc_mutex);

    int slot = -1;
    for (int i = 0; i < ALLOC_TRACK_MAX; i++) {
        if (!g_alloc_tracker.entries[i].active) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        alloc_entry_t *e = &g_alloc_tracker.entries[slot];
        e->ptr = ptr;
        e->size = size;
        e->file = __FILE__;
        e->line = __LINE__;
        e->active = 1;
        g_alloc_tracker.count++;
    }

    os_mutex_unlock(g_alloc_mutex);
    return ptr;
}

void framework_free(void *ptr)
{
    if (!ptr) return;

    os_mutex_lock(g_alloc_mutex);

    for (int i = 0; i < ALLOC_TRACK_MAX; i++) {
        if (g_alloc_tracker.entries[i].active &&
            g_alloc_tracker.entries[i].ptr == ptr) {
            g_alloc_tracker.entries[i].active = 0;
            g_alloc_tracker.count--;
            break;
        }
    }

    os_mutex_unlock(g_alloc_mutex);
    os_free(ptr);
}

void framework_leak_report(void)
{
    int leaks = 0;

    LOG_INFO("%s", "========================================");
    LOG_INFO("%s", "  MEMORY LEAK REPORT");
    LOG_INFO("%s", "========================================");

    os_mutex_lock(g_alloc_mutex);

    for (int i = 0; i < ALLOC_TRACK_MAX; i++) {
        alloc_entry_t *e = &g_alloc_tracker.entries[i];
        if (e->active) {
            leaks++;
            LOG_WARN("  LEAK #%d: %zu bytes at %p (%s:%d)",
                     leaks, e->size, e->ptr, e->file, e->line);
        }
    }

    os_mutex_unlock(g_alloc_mutex);

    if (leaks == 0) {
        LOG_INFO("%s", "  No memory leaks detected.");
    } else {
        LOG_WARN("  Total: %d leaked block(s)", leaks);
    }

    LOG_INFO("%s", "========================================");
}
