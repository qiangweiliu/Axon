/*
 * modules/logger/logger.c — Lock-free async logger.
 *
 * Two modes:
 *   NONE     → printf directly to stderr
 *   BUFFERED → ring buffer, log thread polls every 2ms
 *
 * No mutex, no condvar — only __atomic operations.
 * All OS and C library calls go through os_api.h.
 */

#define _DEFAULT_SOURCE

#include "logger.h"
#include "agent_framework.h"
#include "os_api.h"
#include <stdarg.h>
#include <time.h>
#include <stdatomic.h>

#define MAX_MOD 64
static struct { char name[64]; fw_log_level_t lvl; } g_mods[MAX_MOD];
static _Atomic int g_mod_n;
static fw_log_level_t g_global_lvl = FW_LOG_INFO;

#define RING_N   1024
#define ENTRY_N  4096

static char g_ring[RING_N][ENTRY_N];
static _Atomic unsigned g_rb_h;
static _Atomic unsigned g_rb_t;
static _Atomic unsigned g_rb_c;

static fw_log_backend_t g_be = FW_LOG_BACKEND_NONE;
static os_file_handle_t g_flog = NULL;
static _Atomic int g_active;
static os_thread_handle_t g_tid;
static const char *g_cur_mod;

static const char *lvl_str(fw_log_level_t l)
{
    switch (l) {
    case FW_LOG_DEBUG: return "DEBUG";
    case FW_LOG_INFO:  return "INFO ";
    case FW_LOG_WARN:  return "WARN ";
    case FW_LOG_ERROR: return "ERROR";
    case FW_LOG_FATAL: return "FATAL";
    default:           return "???";
    }
}

static void rb_push(const char *s, size_t n)
{
    unsigned slot;
    for (;;) {
        unsigned h = atomic_load_explicit(&g_rb_h, memory_order_relaxed);
        unsigned c = atomic_load_explicit(&g_rb_c, memory_order_relaxed);
        if (c >= RING_N) {
            unsigned t = atomic_load_explicit(&g_rb_t, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&g_rb_t, &t, (t+1) % RING_N,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                atomic_fetch_sub_explicit(&g_rb_c, 1, memory_order_relaxed);
                continue;
            }
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&g_rb_h, &h, (h+1) % RING_N,
                                                   memory_order_release, memory_order_relaxed)) {
            slot = h;
            break;
        }
    }

    if (n >= ENTRY_N) n = ENTRY_N - 1;
    os_memcpy(g_ring[slot], s, n);
    g_ring[slot][n] = '\0';
    atomic_fetch_add_explicit(&g_rb_c, 1, memory_order_release);
}

static void do_printf(fw_log_level_t l, const char *mod, const char *fmt, va_list ap)
{
    if (!mod || os_strcmp(mod, "(none)") == 0) mod = "(none)";
    os_fprintf_stderr("[%s] %-16s ", lvl_str(l), mod);
    /* vfprintf not available via os_api, use fallback */
    char tmp[512];
    int n = os_vsprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        os_fprintf_stderr("%.*s\n", n, tmp);
    }
}

static void *log_thread(void *arg)
{
    (void)arg;
    while (atomic_load_explicit(&g_active, memory_order_acquire)) {
        os_sleep_ms(2);
        for (;;) {
            unsigned c = atomic_load_explicit(&g_rb_c, memory_order_acquire);
            if (c == 0) break;
            unsigned t = atomic_load_explicit(&g_rb_t, memory_order_relaxed);
            const char *entry = g_ring[t];
            size_t n = os_strlen(entry);
            if (n == 0) break;
            if (!atomic_compare_exchange_weak_explicit(&g_rb_t, &t, (t+1) % RING_N,
                                                        memory_order_relaxed, memory_order_relaxed))
                continue;
            atomic_fetch_sub_explicit(&g_rb_c, 1, memory_order_release);
            if (g_flog) {
                os_file_write(g_flog, entry, n);
                os_file_write(g_flog, "\n", 1);
            } else {
                os_fprintf_stderr("%s\n", entry);
            }
        }
    }

    /* Final drain */
    while (1) {
        unsigned c = atomic_load_explicit(&g_rb_c, memory_order_acquire);
        if (c == 0) break;
        unsigned t = atomic_load_explicit(&g_rb_t, memory_order_relaxed);
        const char *entry = g_ring[t];
        size_t n = os_strlen(entry);
        if (n == 0) break;
        if (!atomic_compare_exchange_weak_explicit(&g_rb_t, &t, (t+1) % RING_N,
                                                    memory_order_relaxed, memory_order_relaxed))
            continue;
        atomic_fetch_sub_explicit(&g_rb_c, 1, memory_order_release);
        os_fprintf_stderr("%s\n", entry);
    }
    return NULL;
}

void fw_log_switch(fw_log_backend_t backend)
{
    if (backend == g_be) return;

    if (backend == FW_LOG_BACKEND_BUFFERED) {
        if (!atomic_load_explicit(&g_active, memory_order_acquire)) {
            atomic_store_explicit(&g_active, 1, memory_order_release);
            os_thread_create(&g_tid, log_thread, NULL);
            os_thread_detach(g_tid);
        }
    } else {
        while (1) {
            unsigned c = atomic_load_explicit(&g_rb_c, memory_order_acquire);
            if (c == 0) break;
            unsigned t = atomic_load_explicit(&g_rb_t, memory_order_relaxed);
            const char *entry = g_ring[t];
            size_t n = os_strlen(entry);
            if (n == 0) break;
            if (!atomic_compare_exchange_weak_explicit(&g_rb_t, &t, (t+1) % RING_N,
                                                        memory_order_relaxed, memory_order_relaxed))
                continue;
            atomic_fetch_sub_explicit(&g_rb_c, 1, memory_order_release);
            os_fprintf_stderr("%s\n", entry);
        }
        if (atomic_load_explicit(&g_active, memory_order_acquire)) {
            atomic_store_explicit(&g_active, 0, memory_order_release);
            os_thread_join(g_tid);
        }
    }

    g_be = backend;
}

fw_log_backend_t fw_log_get_backend(void)
{
    return g_be;
}

int fw_log_init(const char *log_file, fw_log_level_t lvl)
{
    g_global_lvl = lvl;
    if (log_file) {
        g_flog = os_file_open(log_file, "a");
        if (!g_flog) {
            char msg[256];
            os_snprintf(msg, sizeof(msg), "Logger: can't open '%s'\n", log_file);
            os_fprintf_stderr("%s", msg);
        }
    }
    fw_log_switch(FW_LOG_BACKEND_BUFFERED);
    return 0;
}

int fw_log_set_level(const char *name, fw_log_level_t lvl)
{
    int n = atomic_load_explicit(&g_mod_n, memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (os_strcmp(g_mods[i].name, name) == 0) { g_mods[i].lvl = lvl; return 0; }
    if (n < MAX_MOD) {
        os_strncpy(g_mods[n].name, name, 63);
        g_mods[n].name[63] = '\0';
        g_mods[n].lvl = lvl;
        atomic_store_explicit(&g_mod_n, n + 1, memory_order_release);
    }
    return 0;
}

void fw_log_shutdown(void)
{
    fw_log_switch(FW_LOG_BACKEND_NONE);
    if (g_flog) { os_file_close(g_flog); g_flog = NULL; }
}

void _fw_log(fw_log_level_t lvl, const char *mod, const char *fmt, ...)
{
    if (lvl < g_global_lvl) return;

    fw_log_level_t eff = g_global_lvl;
    if (mod && os_strcmp(mod, "(none)") != 0 && os_strcmp(mod, "(unknown)") != 0) {
        int n = atomic_load_explicit(&g_mod_n, memory_order_acquire);
        for (int i = 0; i < n; i++)
            if (os_strcmp(g_mods[i].name, mod) == 0) { eff = g_mods[i].lvl; break; }
    }
    if (lvl < eff) return;

    if (g_be == FW_LOG_BACKEND_NONE) {
        va_list ap; os_va_start(ap, fmt); do_printf(lvl, mod, fmt, ap); os_va_end(ap);
        return;
    }

    char buf[ENTRY_N];
    char ts[32];
    time_t t = time(NULL);
    os_time_format(ts, sizeof(ts), t);

    if (!mod || os_strcmp(mod, "(none)") == 0) mod = "(none)";

    int pl = os_snprintf(buf, sizeof(buf), "[%s] [%-16s] %-7s ", ts, mod, lvl_str(lvl));
    va_list ap; os_va_start(ap, fmt);
    int ml = os_vsprintf(buf + pl, sizeof(buf) - (size_t)pl, fmt, ap);
    os_va_end(ap);
    if (ml < 0 || pl + ml >= (int)sizeof(buf)) ml = (int)sizeof(buf) - pl - 1;

    rb_push(buf, (size_t)(pl + ml));
}

void fw_log_bind_module(const char *name)
{
    g_cur_mod = name ? name : "(none)";
}

const char *fw_log_get_module(void)
{
    return g_cur_mod ? g_cur_mod : "(none)";
}

/* ── Module registration ─────────────────────────────────────────── */

static int logger_init(struct framework_module *m) { (void)m; LOG_INFO("Logger: init"); return 0; }
static int logger_start(struct framework_module *m) { (void)m; LOG_INFO("Logger: ready"); return 0; }

struct framework_module logger_mod = {
        .layer = LAYER_CORE,
    .offset = 0,
    .name = "logger", .version = 0x00010000,  .state = 0,
    .init = logger_init, .start = logger_start,
    .loop = NULL, .stop = NULL, .deinit = NULL, .ctx = NULL,
};

MODULE_REGISTER(logger_mod);
