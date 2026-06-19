/*
 * memory.h — Memory management public API
 *
 * Business layer (priority=400). Backend-agnostic — swap backends
 * by implementing memory_backend_t and calling memory_set_backend().
 *
 * Default backend: TSV file (backend_file.c).
 */

#ifndef BUSINESS_MEMORY_H
#define BUSINESS_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define MEMORY_ID_MAX     64
#define MEMORY_TYPE_MAX   32
#define MEMORY_CONTENT_MAX 8192
#define MEMORY_META_MAX   512

/* A single memory entry */
typedef struct {
    char     id[MEMORY_ID_MAX];
    char     type[MEMORY_TYPE_MAX];    /* "fact", "conversation", "knowledge" */
    uint64_t timestamp;
    char     content[MEMORY_CONTENT_MAX];
    char     metadata[MEMORY_META_MAX]; /* JSON key-value extras */
} memory_entry_t;

/*
 * Backend interface — implement this struct to plug in a new backend.
 * All functions return 0 on success, -1 on failure.
 */
typedef struct memory_backend {
    int (*init)(void);
    int (*shutdown)(void);

    int (*store)(const memory_entry_t *entry, char *id_out, size_t id_len);
    int (*retrieve)(const char *id, memory_entry_t *entry_out);
    int (*update)(const char *id, const memory_entry_t *entry);
    int (*remove)(const char *id);

    int (*search)(const char *query, memory_entry_t *results, int max_results,
                  int *out_count);
    int (*count)(void);
    int (*clear)(void);
} memory_backend_t;

/* ── Public API (dispatches to active backend) ────────────────────── */

/* Set the active backend. Call before any other memory_* function.
   If never called, the default file backend (backend_file.c) is used. */
void memory_set_backend(const memory_backend_t *backend);

/* Store a memory entry. id_out receives the generated ID. */
int memory_store(const memory_entry_t *entry, char *id_out, size_t id_len);

/* Retrieve a memory entry by ID. */
int memory_retrieve(const char *id, memory_entry_t *entry_out);

/* Update an existing entry (matched by ID in the entry). */
int memory_update(const memory_entry_t *entry);

/* Remove an entry by ID. */
int memory_remove(const char *id);

/*
 * Search entries by substring match on content and type.
 * results: caller-allocated array of memory_entry_t (max_results elements).
 * out_count: receives actual number of matches (≤ max_results).
 */
int memory_search(const char *query, memory_entry_t *results,
                  int max_results, int *out_count);

/* Number of stored entries. */
int memory_count(void);

/* Remove all entries. */
int memory_clear(void);

#endif /* BUSINESS_MEMORY_H */
