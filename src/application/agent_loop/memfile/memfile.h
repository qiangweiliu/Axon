/* memfile.h — Bounded markdown memory files (Hermes-style MEMORY.md / USER.md)
 *
 * Each file stores entries separated by "\n§\n" delimiters.
 * Injected into the REPL at startup as persistent context.
 * Capped at configurable char limits — agent must consolidate when full.
 *
 * Design aligns with Hermes Agent's memory tool:
 *   add     — memfile_add()
 *   replace — memfile_replace()
 *   remove  — memfile_remove()
 *
 * Two stores: memory.md (agent notes) and user.md (user profile).
 */

#ifndef AGENT_MEMFILE_H
#define AGENT_MEMFILE_H

#include <stddef.h>

#define MEMFILE_MAX_ENTRIES   64        /* max number of entries in one file */
#define MEMFILE_ENTRY_MAX     1024      /* max chars per entry             */
#define MEMFILE_PATH_MAX      256

typedef struct {
    char  entries[MEMFILE_MAX_ENTRIES][MEMFILE_ENTRY_MAX];
    int   count;
    int   total_chars;        /* current total (content + separators) */
    int   limit;              /* total char ceiling (e.g. 2200)       */
    char  path[MEMFILE_PATH_MAX];
} memfile_t;

/* Load a memory file from disk. Creates empty if not exist. */
void memfile_load(const char *path, memfile_t *mf, int limit);

/* Add an entry. Returns 0 on success, -1 if would exceed limit. */
int  memfile_add(memfile_t *mf, const char *text);

/*
 * Replace: find the first entry whose content contains old_substr,
 * and replace it with new_text. Returns 0 on success, -1 if:
 *   - no entry matches old_substr
 *   - new_text would exceed the char limit
 */
int  memfile_replace(memfile_t *mf, const char *old_substr,
                     const char *new_text);

/* Remove entries whose content contains substring. Returns # removed. */
int  memfile_remove(memfile_t *mf, const char *substring);

/* Write current state back to disk. */
void memfile_save(const memfile_t *mf);

/* Get usage string like "45% — 990/2200 chars" (into buf). */
void memfile_usage(const memfile_t *mf, char *buf, size_t buf_len);

#endif /* AGENT_MEMFILE_H */
