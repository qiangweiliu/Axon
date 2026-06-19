/* memfile.c — Bounded memory file implementation (Hermes-style).
 *
 * File format (same as Hermes MEMORY.md / USER.md):
 *   Entry 1 content
 *   §
 *   Entry 2 content
 *   §
 *   Entry 3 content
 *
 * On disk, entries separated by "\n§\n". total_chars = Σlen(entry_i) + (count-1)*3.
 */

#include "memfile.h"
#include "os_api.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* ── Internal helpers ────────────────────────────────────────────── */

/* Recalculate total_chars from scratch (always consistent) */
static void recalc_total(memfile_t *mf)
{
    int total = 0;
    for (int i = 0; i < mf->count; i++) {
        total += (int)os_strlen(mf->entries[i]);
        if (i > 0) total += 3;   /* "\n§\n" between entries */
    }
    mf->total_chars = total;
}

/* Parse raw file content into memory entries */
static void parse_entries(const char *raw, memfile_t *mf)
{
    mf->count = 0;
    mf->total_chars = 0;

    const char *p = raw;
    while (*p && mf->count < MEMFILE_MAX_ENTRIES) {
        /* Skip leading whitespace / blank lines */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p) break;

        const char *start = p;               /* start of entry content */
        const char *end   = strstr(p, "\n§\n");

        if (!end) {
            /* Last (or only) entry — go to end of string */
            end = p + strlen(p);
        } else {
            p = end + 3;  /* skip past "\n§\n" for next iteration */
        }

        /* Trim trailing whitespace from the entry content */
        while (end > start &&
               (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            end--;

        size_t len = (size_t)(end - start);
        if (len == 0) continue;               /* skip empty entry */

        if (len >= MEMFILE_ENTRY_MAX)
            len = MEMFILE_ENTRY_MAX - 1;

        os_memcpy(mf->entries[mf->count], start, len);
        mf->entries[mf->count][len] = '\0';
        mf->count++;
    }

    /* Recalculate for guaranteed consistency */
    recalc_total(mf);
}

/* ── Public API ──────────────────────────────────────────────────── */

void memfile_load(const char *path, memfile_t *mf, int limit)
{
    os_memset(mf, 0, sizeof(*mf));
    mf->limit = limit;

    size_t plen = os_strlen(path);
    if (plen >= sizeof(mf->path))
        plen = sizeof(mf->path) - 1;
    os_memcpy(mf->path, path, plen);
    mf->path[plen] = '\0';

    /* Create parent directory if needed */
    {
        char dir[MEMFILE_PATH_MAX];
        size_t dl = plen;
        while (dl > 0 && path[dl - 1] != '/')
            dl--;
        if (dl > 0 && dl < sizeof(dir)) {
            os_memcpy(dir, path, dl);
            dir[dl] = '\0';
            os_dir_create(dir);
        }
    }

    os_file_handle_t fh = os_file_open(path, "r");
    if (!fh) return;   /* file doesn't exist yet — empty is fine */

    char buf[8192];
    size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
    os_file_close(fh);

    if (n > 0) {
        buf[n] = '\0';
        parse_entries(buf, mf);
    }
}

int memfile_add(memfile_t *mf, const char *text)
{
    if (!text || !*text) return -1;

    size_t tlen = os_strlen(text);
    if (tlen >= MEMFILE_ENTRY_MAX)
        tlen = MEMFILE_ENTRY_MAX - 1;

    /* Estimate: existing total + new entry + separator if not first */
    int add_cost = (int)tlen + (mf->count > 0 ? 3 : 0);
    if (mf->total_chars + add_cost > mf->limit)
        return -1;

    if (mf->count >= MEMFILE_MAX_ENTRIES)
        return -1;

    os_memcpy(mf->entries[mf->count], text, tlen);
    mf->entries[mf->count][tlen] = '\0';
    mf->count++;

    recalc_total(mf);
    return 0;
}

int memfile_replace(memfile_t *mf, const char *old_substr,
                    const char *new_text)
{
    if (!old_substr || !new_text) return -1;

    /* Find the first entry containing old_substr */
    int idx = -1;
    for (int i = 0; i < mf->count; i++) {
        if (strstr(mf->entries[i], old_substr)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;   /* not found */

    /* Estimate: subtract old entry cost, add new entry cost */
    int old_len = (int)os_strlen(mf->entries[idx]);
    int old_cost = old_len + (idx > 0 ? 3 : 0);

    int new_len = (int)os_strlen(new_text);
    if (new_len >= MEMFILE_ENTRY_MAX)
        new_len = MEMFILE_ENTRY_MAX - 1;
    int new_cost = new_len + (idx > 0 ? 3 : 0);

    if (mf->total_chars - old_cost + new_cost > mf->limit)
        return -1;   /* would exceed limit */

    /* Replace in-place */
    os_memcpy(mf->entries[idx], new_text, (size_t)new_len);
    mf->entries[idx][new_len] = '\0';

    recalc_total(mf);
    return 0;
}

int memfile_remove(memfile_t *mf, const char *substring)
{
    if (!substring) return 0;

    int removed = 0;
    int i = 0;
    while (i < mf->count) {
        if (strstr(mf->entries[i], substring)) {
            /* Shift remaining entries left */
            for (int j = i; j < mf->count - 1; j++)
                os_memcpy(mf->entries[j], mf->entries[j + 1],
                          MEMFILE_ENTRY_MAX);
            mf->count--;
            removed++;
            /* Don't increment i — the entry that shifted into this
               position needs checking too */
        } else {
            i++;
        }
    }

    if (removed > 0)
        recalc_total(mf);

    return removed;
}

void memfile_save(const memfile_t *mf)
{
    os_file_handle_t fh = os_file_open(mf->path, "w");
    if (!fh) return;

    for (int i = 0; i < mf->count; i++) {
        if (i > 0)
            os_file_write(fh, "\n§\n", 3);
        size_t len = os_strlen(mf->entries[i]);
        os_file_write(fh, mf->entries[i], len);
    }

    os_file_close(fh);
}

void memfile_usage(const memfile_t *mf, char *buf, size_t buf_len)
{
    int pct = mf->limit > 0 ? (mf->total_chars * 100 / mf->limit) : 0;
    if (pct > 99) pct = 99;

    /* Human-readable size */
    int used_k = (mf->total_chars + 512) / 1024;  /* round to nearest K */
    int limit_k = (mf->limit + 512) / 1024;

    /* Progress bar: 10 blocks */
    char bar[11];
    int fill = pct / 10;  /* 0..9 */
    for (int i = 0; i < 10; i++)
        bar[i] = (i < fill) ? '#' : '-';
    bar[10] = '\0';

    if (limit_k >= 1) {
        os_snprintf(buf, buf_len, "%dK/%dK [%s] %d%%",
                    used_k, limit_k, bar, pct);
    } else {
        os_snprintf(buf, buf_len, "%d/%d [%s] %d%%",
                    mf->total_chars, mf->limit, bar, pct);
    }
}
