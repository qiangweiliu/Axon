/*
 * backend_file.c — Default file-based memory backend
 *
 * Stores entries as TSV lines in data/memory.db.
 * Simple, portable, no external dependencies.
 * Good for up to ~10K entries.
 *
 * To swap backends: implement memory_backend_t, call memory_set_backend().
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "memory.h"

#define DB_PATH         "data/memory.db"
#define DB_PATH_TMP     "data/memory.db.tmp"
#define LINE_MAX        12288   /* MEMORY_CONTENT_MAX + overhead */

typedef struct {
    os_mutex_handle_t db_mutex;
} backend_file_ctx_t;

static backend_file_ctx_t *g_ctx = NULL;

/* ── Escape / Unescape ────────────────────────────────────────────── */

static void escape(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (const char *s = src; *s && j < dst_len - 2; s++) {
        if (*s == '\\')      { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (*s == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (*s == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else                  { dst[j++] = *s; }
    }
    dst[j] = '\0';
}

static void unescape(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    while (*src && j < dst_len - 1) {
        if (*src == '\\' && *(src+1)) {
            src++;
            if (*src == 't') dst[j++] = '\t';
            else if (*src == 'n') dst[j++] = '\n';
            else dst[j++] = *src;
        } else {
            dst[j++] = *src;
        }
        src++;
    }
    dst[j] = '\0';
}

/* ── ID Generation ────────────────────────────────────────────────── */

static void gen_id(char *buf, size_t len)
{
    static int counter;
    uint64_t ts = os_clock_ms();
    os_snprintf(buf, len, "%llu-%d", (unsigned long long)ts, ++counter);
}

/* ── TSV Line Read/Write ──────────────────────────────────────────── */

static int entry_from_line(const char *line, memory_entry_t *entry)
{
    /* Parse: id\ttype\ttimestamp\tcontent\tmetadata */
    /* We modify the line in-place to NUL-terminate each field */
    char buf[LINE_MAX];
    os_memcpy(buf, line, os_strlen(line) + 1);
    char *p = buf;

    const char *fields[5];
    int n = 0;

    for (int i = 0; i < 5; i++) {
        fields[i] = p;
        while (*p && *p != '\t') p++;
        char saved = *p;
        *p = '\0';
        n++;
        if (saved == '\t') {
            p++;
        } else {
            while (n < 5) fields[n++] = "";
            break;
        }
    }

    if (n < 5) return -1;

    os_memset(entry, 0, sizeof(*entry));

    /* ID */
    size_t len = os_strlen(fields[0]);
    if (len >= MEMORY_ID_MAX) len = MEMORY_ID_MAX - 1;
    os_memcpy(entry->id, fields[0], len);

    /* Type */
    len = os_strlen(fields[1]);
    if (len >= MEMORY_TYPE_MAX) len = MEMORY_TYPE_MAX - 1;
    os_memcpy(entry->type, fields[1], len);

    /* Timestamp */
    entry->timestamp = 0;
    for (const char *c = fields[2]; *c >= '0' && *c <= '9'; c++)
        entry->timestamp = entry->timestamp * 10 + (uint64_t)(*c - '0');

    /* Content (unescape) */
    unescape(entry->content, MEMORY_CONTENT_MAX, fields[3]);

    /* Metadata (unescape) */
    unescape(entry->metadata, MEMORY_META_MAX, fields[4]);

    return 0;
}

static int entry_to_line(const memory_entry_t *entry, char *line, size_t len)
{
    char esc_content[MEMORY_CONTENT_MAX * 2];
    char esc_meta[MEMORY_META_MAX * 2];

    escape(esc_content, sizeof(esc_content), entry->content);
    escape(esc_meta, sizeof(esc_meta), entry->metadata);

    return os_snprintf(line, len, "%s\t%s\t%llu\t%s\t%s\n",
                       entry->id, entry->type,
                       (unsigned long long)entry->timestamp,
                       esc_content, esc_meta);
}

/* ── Backend Implementation ───────────────────────────────────────── */

static int file_init(void)
{
    /* Ensure data directory exists */
    os_dir_create("data");

    os_file_handle_t fh = os_file_open(DB_PATH, "a");
    if (fh) { os_file_close(fh); }
    if (!g_ctx) {
        g_ctx = (backend_file_ctx_t *)os_calloc(1, sizeof(backend_file_ctx_t));
        if (!g_ctx) return -1;
    }
    g_ctx->db_mutex = os_mutex_create();
    return g_ctx->db_mutex ? 0 : -1;
}

static int file_shutdown(void)
{
    if (g_ctx->db_mutex) { os_mutex_destroy(g_ctx->db_mutex); g_ctx->db_mutex = NULL; }
    return 0;
}

static int file_store(const memory_entry_t *entry, char *id_out, size_t id_len)
{
    if (!entry) return -1;

    memory_entry_t e = *entry;
    if (e.id[0] == '\0') gen_id(e.id, sizeof(e.id));
    if (e.timestamp == 0) e.timestamp = os_clock_ms();

    char line[LINE_MAX];
    int n = entry_to_line(&e, line, sizeof(line));
    if (n <= 0) return -1;

    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "a");
    if (!fh) { os_mutex_unlock(g_ctx->db_mutex); return -1; }
    os_file_write(fh, line, (size_t)n);
    os_file_close(fh);
    os_mutex_unlock(g_ctx->db_mutex);

    if (id_out && id_len > 0) {
        size_t elen = os_strlen(e.id);
        if (elen >= id_len) elen = id_len - 1;
        os_memcpy(id_out, e.id, elen);
        id_out[elen] = '\0';
    }

    return 0;
}

static int file_retrieve(const char *id, memory_entry_t *entry_out)
{
    if (!id || !entry_out) return -1;

    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "r");
    if (!fh) { os_mutex_unlock(g_ctx->db_mutex); return -1; }

    char line[LINE_MAX];
    size_t pos = 0;
    int found = 0;

    while (1) {
        size_t nr = os_file_read(fh, &line[pos], 1);
        if (nr == 0) break;
        if (line[pos] == '\n' || pos >= LINE_MAX - 2) {
            line[pos] = '\0';
            memory_entry_t tmp;
            if (entry_from_line(line, &tmp) == 0) {
                if (os_strcmp(tmp.id, id) == 0) {
                    *entry_out = tmp;
                    found = 1;
                    break;
                }
            }
            pos = 0;
        } else {
            pos++;
        }
    }

    os_file_close(fh);
    os_mutex_unlock(g_ctx->db_mutex);
    return found ? 0 : -1;
}

static int file_update(const char *id, const memory_entry_t *entry)
{
    /* Read all, rewrite file replacing matching entry */
    if (!id || !entry) return -1;

    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "r");
    if (!fh) { os_mutex_unlock(g_ctx->db_mutex); return -1; }

    char *buf = (char *)os_alloc(65536);
    if (!buf) { os_file_close(fh); os_mutex_unlock(g_ctx->db_mutex); return -1; }
    size_t total = os_file_read(fh, buf, 65535);
    os_file_close(fh);
    buf[total] = '\0';

    /* Rewrite to temp file */
    os_file_handle_t fout = os_file_open(DB_PATH_TMP, "w");
    if (!fout) { os_free(buf); os_mutex_unlock(g_ctx->db_mutex); return -1; }

    char new_line[LINE_MAX];
    memory_entry_t e = *entry;
    if (e.timestamp == 0) e.timestamp = os_clock_ms();
    os_memcpy(e.id, id, os_strlen(id) + 1);
    int nlen = entry_to_line(&e, new_line, sizeof(new_line));

    int found = 0;
    const char *p = buf;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t llen = (size_t)(p - line_start);
        char saved = *p;

        char tmp_line[LINE_MAX];
        size_t cp = llen < LINE_MAX ? llen : LINE_MAX - 1;
        os_memcpy(tmp_line, line_start, cp);
        tmp_line[cp] = '\0';

        memory_entry_t tmp;
        if (entry_from_line(tmp_line, &tmp) == 0 &&
            os_strcmp(tmp.id, id) == 0) {
            if (nlen > 0) os_file_write(fout, new_line, (size_t)nlen);
            found = 1;
        } else {
            os_file_write(fout, line_start, llen);
            if (saved == '\n') os_file_write(fout, "\n", 1);
        }

        if (saved == '\n') p++;
    }

    /* If not found, append as new */
    if (!found && nlen > 0) {
        os_file_write(fout, new_line, (size_t)nlen);
    }

    os_file_close(fout);
    os_free(buf);

    /* Atomic rename */
    os_file_handle_t tmp_fh = os_file_open(DB_PATH_TMP, "r");
    os_file_handle_t dst_fh = os_file_open(DB_PATH, "w");
    if (tmp_fh && dst_fh) {
        char rbuf[4096];
        size_t nr;
        while ((nr = os_file_read(tmp_fh, rbuf, sizeof(rbuf))) > 0)
            os_file_write(dst_fh, rbuf, nr);
    }
    if (tmp_fh) os_file_close(tmp_fh);
    if (dst_fh) os_file_close(dst_fh);

    os_mutex_unlock(g_ctx->db_mutex);
    return 0;
}

static int file_remove(const char *id)
{
    if (!id) return -1;

    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "r");
    if (!fh) { os_mutex_unlock(g_ctx->db_mutex); return -1; }

    char *buf = (char *)os_alloc(65536);
    if (!buf) { os_file_close(fh); os_mutex_unlock(g_ctx->db_mutex); return -1; }
    size_t total = os_file_read(fh, buf, 65535);
    os_file_close(fh);
    buf[total] = '\0';

    os_file_handle_t fout = os_file_open(DB_PATH_TMP, "w");
    if (!fout) { os_free(buf); os_mutex_unlock(g_ctx->db_mutex); return -1; }

    const char *p = buf;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t llen = (size_t)(p - line_start);
        char saved = *p;

        char tmp_line[LINE_MAX];
        size_t cp = llen < LINE_MAX ? llen : LINE_MAX - 1;
        os_memcpy(tmp_line, line_start, cp);
        tmp_line[cp] = '\0';

        memory_entry_t tmp;
        if (!(entry_from_line(tmp_line, &tmp) == 0 &&
              os_strcmp(tmp.id, id) == 0)) {
            os_file_write(fout, line_start, llen);
            if (saved == '\n') os_file_write(fout, "\n", 1);
        }

        if (saved == '\n') p++;
    }

    os_file_close(fout);
    os_free(buf);

    /* Atomic rename */
    os_file_handle_t tmp_fh = os_file_open(DB_PATH_TMP, "r");
    os_file_handle_t dst_fh = os_file_open(DB_PATH, "w");
    if (tmp_fh && dst_fh) {
        char rbuf[4096];
        size_t nr;
        while ((nr = os_file_read(tmp_fh, rbuf, sizeof(rbuf))) > 0)
            os_file_write(dst_fh, rbuf, nr);
    }
    if (tmp_fh) os_file_close(tmp_fh);
    if (dst_fh) os_file_close(dst_fh);

    os_mutex_unlock(g_ctx->db_mutex);
    return 0;
}

static int file_search(const char *query, memory_entry_t *results,
                       int max_results, int *out_count)
{
    if (!query || !results || max_results < 1 || !out_count) return -1;
    *out_count = 0;

    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "r");
    if (!fh) { os_mutex_unlock(g_ctx->db_mutex); *out_count = 0; return 0; }

    char line[LINE_MAX];
    size_t pos = 0;

    while (*out_count < max_results) {
        size_t nr = os_file_read(fh, &line[pos], 1);
        if (nr == 0) break;
        if (line[pos] == '\n' || pos >= LINE_MAX - 2) {
            line[pos] = '\0';
            memory_entry_t tmp;
            if (entry_from_line(line, &tmp) == 0) {
                /* Match: empty query = all, type exact match, or content substring */
                int match = (os_strlen(query) == 0);
                if (!match && os_strcmp(tmp.type, query) == 0) {
                    match = 1;
                }
                if (!match) {
                    for (const char *c = tmp.content; *c; c++) {
                        const char *q = query;
                        const char *s = c;
                        while (*q && *s && *q == *s) { q++; s++; }
                        if (!*q) { match = 1; break; }
                    }
                }
                if (match) {
                    results[*out_count] = tmp;
                    (*out_count)++;
                }
            }
            pos = 0;
        } else {
            pos++;
        }
    }

    os_file_close(fh);
    os_mutex_unlock(g_ctx->db_mutex);
    return 0;
}

static int file_count(void)
{
    int n = 0;
    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "r");
    if (fh) {
        char c;
        while (os_file_read(fh, &c, 1) > 0)
            if (c == '\n') n++;
        os_file_close(fh);
    }
    os_mutex_unlock(g_ctx->db_mutex);
    return n;
}

static int file_clear(void)
{
    os_mutex_lock(g_ctx->db_mutex);
    os_file_handle_t fh = os_file_open(DB_PATH, "w");
    if (fh) os_file_close(fh);
    os_mutex_unlock(g_ctx->db_mutex);
    return 0;
}

/* ── Backend Definition ───────────────────────────────────────────── */

const memory_backend_t memory_backend_file = {
    .init     = file_init,
    .shutdown = file_shutdown,
    .store    = file_store,
    .retrieve = file_retrieve,
    .update   = file_update,
    .remove   = file_remove,
    .search   = file_search,
    .count    = file_count,
    .clear    = file_clear,
};
