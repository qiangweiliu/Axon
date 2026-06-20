/*
 * log_store.c — L5 archive log (data/memory/l5/archive/)
 *
 * Execution Layer.
 *
 * Structure (each event = individual file + global index):
 *   data/memory/l5/archive/
 *   ├── index.tsv              ← global keyword index
 *   └── {date}/
 *       ├── {ts}-{seq:03}.event
 *       ├── {ts}-{seq:03}.event
 *       └── ...
 *
 * Benefits:
 *   - Append is O(1): write 1 event file + 1 index line
 *   - Search is O(index): scan index (small), read only matching files
 *   - No full-log scans, no seek/tell needed
 *   - Output bounded to 3 most recent matching events
 */

#include "agent_framework.h"
#include "os_api.h"
#include "archive.h"
#include "keywords.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define RECALL_MAX  800   /* max chars for L5 content in prompt */
#define IDX_PATH    ARC_ARCHIVE_DIR "/index.tsv"
#define LINE_MAX    8192

/* ── Global index I/O ────────────────────────────────────────────── */

/* Append one line to index: keyword\tfilename\tdate */
static void idx_append(const char *keywords, const char *filename, const char *date)
{
    os_file_handle_t fh = os_file_open(IDX_PATH, "a");
    if (!fh) return;
    char line[512];
    int n = os_snprintf(line, sizeof(line), "%s\t%s\t%s\n",
                        keywords, filename, date);
    if (n > 0) os_file_write(fh, line, (size_t)n);
    os_file_close(fh);
}

/* Read all index lines into a buffer. Returns number of lines. */
static int idx_read_all(char *buf, size_t buflen, const char **lines, int max_lines)
{
    os_file_handle_t fh = os_file_open(IDX_PATH, "r");
    if (!fh) return 0;

    size_t n = os_file_read(fh, buf, buflen - 1);
    os_file_close(fh);
    if (n == 0) return 0;
    buf[n] = '\0';

    int count = 0;
    char *p = buf;
    while (*p && count < max_lines) {
        lines[count++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }
    return count;
}

/* ── Extract keywords from question (first 5, for index) ─────────── */

static void idx_keywords(const char *question, char *result, size_t rlen)
{
    char all[256];
    kw_extract(question, all, sizeof(all));
    if (!all[0]) { result[0] = '\0'; return; }

    int count = 0;
    size_t pos = 0;
    const char *p = all;
    while (*p && count < 5 && pos < rlen - 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *end = strchr(p, ' ');
        if (!end) end = p + os_strlen(p);
        size_t wlen = (size_t)(end - p);
        if (count > 0 && pos < rlen - 1) result[pos++] = ' ';
        if (pos + wlen < rlen - 1) {
            os_memcpy(result + pos, p, wlen);
            pos += wlen;
        }
        count++;
        p = end;
    }
    result[pos] = '\0';
}

/* ── Append — write event file + index entry ─────────────────────── */

int log_store_append(const char *session_id,
                     const char *question, const char *answer)
{
    (void)session_id;

    time_t now_t = time(NULL);
    struct tm *tm = localtime(&now_t);

    /* Create dated subdirectory */
    char datedir[128], event_path[512];
    os_snprintf(datedir, sizeof(datedir), "%s/%04d-%02d-%02d",
                ARC_ARCHIVE_DIR, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    os_dir_create(datedir);

    /* Generate unique event filename from timestamp + sequence */
    static int seq = 0;
    seq++;
    os_snprintf(event_path, sizeof(event_path), "%s/%02d%02d%02d-%03d.event",
                datedir, tm->tm_hour, tm->tm_min, tm->tm_sec,
                seq % 1000);

    /* Write event content */
    char content[LINE_MAX];
    int clen = os_snprintf(content, sizeof(content),
        "[%04d-%02d-%02d %02d:%02d:%02d]\nQ: %s\nA: %s\n",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec,
        question ? question : "", answer ? answer : "");
    if (clen <= 0) return -1;

    os_file_handle_t fh = os_file_open(event_path, "w");
    if (!fh) return -1;
    os_file_write(fh, content, (size_t)clen);
    os_file_close(fh);

    /* Extract keywords and update index */
    char kw[128];
    idx_keywords(question, kw, sizeof(kw));
    if (!kw[0]) return 0;

    /* Filename as stored in index (relative to archive root) */
    char relpath[256];
    os_snprintf(relpath, sizeof(relpath), "%04d-%02d-%02d/%s",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                event_path + os_strlen(datedir) + 1);

    char date_str[16];
    os_snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    idx_append(kw, relpath, date_str);
    return 0;
}

/* ── Search — index-assisted, bounded output ─────────────────────── */

/* Read an event file by relative path (e.g. "2026-06-20/103000-001.event") */
static int read_event(const char *relpath, char *buf, size_t buflen)
{
    char full[512];
    os_snprintf(full, sizeof(full), "%s/%s", ARC_ARCHIVE_DIR, relpath);
    os_file_handle_t fh = os_file_open(full, "r");
    if (!fh) return -1;
    size_t n = os_file_read(fh, buf, buflen - 1);
    os_file_close(fh);
    if (n == 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

int log_store_search(const char *query, char *result_buf, size_t result_len)
{
    if (!query || !result_buf || result_len < 1) return -1;

    size_t pos = os_strlen(result_buf);
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "== L5 Archive (raw logs) ==\n");

    /* Read index */
    char idx_buf[16384];
    const char *lines[1024];
    int nlines = idx_read_all(idx_buf, sizeof(idx_buf), lines, 1024);
    if (nlines == 0) {
        pos += os_snprintf(result_buf + pos, result_len - pos,
            "  (no archive index yet)\n");
        return 0;
    }

    /* Scan reverse (newest first) for matching keywords */
    int found = 0;
    for (int i = nlines - 1; i >= 0 && found < 3 && pos < result_len - RECALL_MAX; i--) {
        /* Parse: keyword\tfilename\tdate */
        const char *kw = lines[i];
        const char *tab1 = strchr(kw, '\t');
        if (!tab1) continue;
        const char *filename = tab1 + 1;
        const char *tab2 = strchr(filename, '\t');
        if (!tab2) continue;
        /* Check keyword match */
        size_t kwlen = (size_t)(tab1 - kw);
        char kbuf[128];
        size_t cp = kwlen < sizeof(kbuf) - 1 ? kwlen : sizeof(kbuf) - 1;
        os_memcpy(kbuf, kw, cp);
        kbuf[cp] = '\0';

        if (!strstr(kbuf, query)) continue;

        /* Read matching event file */
        char content[4096];
        if (read_event(filename, content, sizeof(content)) < 0)
            continue;

        /* Extract short Q: line for display */
        char ts[64] = "", q_display[128] = "";
        const char *q_start = strstr(content, "Q: ");
        const char *ts_end = strchr(content, ']');
        if (ts_end) {
            size_t ts_len = (size_t)(ts_end - content - 1);
            if (ts_len > sizeof(ts) - 1) ts_len = sizeof(ts) - 1;
            os_memcpy(ts, content + 1, ts_len);
            ts[ts_len] = '\0';
        }
        if (q_start) {
            const char *q_text = q_start + 3;
            const char *q_end = strchr(q_text, '\n');
            size_t qd = q_end ? (size_t)(q_end - q_text) : os_strlen(q_text);
            if (qd > sizeof(q_display) - 4) qd = sizeof(q_display) - 4;
            os_memcpy(q_display, q_text, qd);
            q_display[qd] = '\0';
        }

        pos += os_snprintf(result_buf + pos, result_len - pos,
            "  [%s] Q: %s\n", ts, q_display);
        found++;
    }

    if (found == 0 && pos < result_len - 50)
        os_snprintf(result_buf + pos, result_len - pos,
            "  (no L5 matches)\n");
    return found;
}

/* Read by timestamp pattern (partial match on [YYYY-MM-DD...) */
int log_store_read_by_ts(const char *ts_pattern,
                         char *result_buf, size_t result_len)
{
    if (!ts_pattern || !result_buf || result_len < 1) return -1;

    char query[128];
    os_snprintf(query, sizeof(query), "%s", ts_pattern);

    size_t pos = 0;
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "== L5 Event at '%s' ==\n", ts_pattern);

    char idx_buf[16384];
    const char *lines[1024];
    int nlines = idx_read_all(idx_buf, sizeof(idx_buf), lines, 1024);

    int found = 0;
    for (int i = nlines - 1; i >= 0 && found < 3 && pos < result_len - RECALL_MAX; i--) {
        const char *filename = lines[i];
        const char *tab1 = strchr(filename, '\t');
        if (!tab1) continue;
        filename = tab1 + 1;
        const char *tab2 = strchr(filename, '\t');
        if (!tab2) continue;

        /* Check date field for timestamp match */
        const char *date = tab2 + 1;
        if (!strstr(date, query) && !strstr(filename, query))
            continue;

        char content[4096];
        if (read_event(filename, content, sizeof(content)) < 0)
            continue;

        size_t clen = os_strlen(content);
        if (clen > 500) {
            content[500] = '\0';
            os_strncpy(content + 500, "\n... (truncated)", sizeof(content) - 500 - 1);
        }
        pos += os_snprintf(result_buf + pos, result_len - pos,
            "  %s\n", content);
        found++;
    }

    if (found == 0)
        pos += os_snprintf(result_buf + pos, result_len - pos,
            "  (no L5 event matching '%s')\n", ts_pattern);
    return found;
}
