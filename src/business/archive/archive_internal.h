/*
 * archive_internal.h — Shared types for archive sub-modules
 *
 * Only included by .c files within src/business/archive/.
 */

#ifndef BUSINESS_ARCHIVE_INTERNAL_H
#define BUSINESS_ARCHIVE_INTERNAL_H

#include "archive.h"
#include <stdint.h>

/* ── Topic entry (L1) — same as archive.h archive_topic_t ────────── */
/* (kept here for internal use; archive.h has the typedef for callers) */

/* ── Archive Context ─────────────────────────────────────────────── */
/* Single global instance, allocated in archive.c, visible to all
   sub-modules via extern. */
typedef struct {
    archive_topic_t topics[ARC_TOPICS_MAX];
    int             topic_count;
    int             topics_dirty;

    /* Cached prompt strings */
    char            topics_prompt[4096];
    int             prompt_dirty;
    char            topics_line[1024];
    int             line_dirty;

    /* Event segmentation — current conversation segment */
    char            seg_topic[128];
    char            seg_turns_q[ARC_SEGMENT_TURNS_MAX][ARC_TURN_TEXT_MAX];
    char            seg_turns_a[ARC_SEGMENT_TURNS_MAX][ARC_TURN_TEXT_MAX];
    int             seg_depth;
    uint64_t        seg_start_ms;
    int             seg_initialized;
} archive_ctx_t;

/* Global context — defined in archive.c, declared here for sub-modules */
extern archive_ctx_t *g_arc;

/* ── Time helpers (shared across sub-modules) ────────────────────── */
uint64_t arc_now_ms(void);
int      arc_days_since(uint64_t timestamp_ms);

/* ── Score calculation (topics.c) ────────────────────────────────── */
int arc_calc_score(const archive_topic_t *t);

/* ── Topics file I/O (topics.c) ──────────────────────────────────── */
void topics_init(void);
void topics_mark_dirty(void);
int  topics_save(void);
int  topics_add(const char *topic, int importance,
                const char *summary, const char *event_id,
                const char *episode_id);
int  topics_find(const char *topic, char *summary, size_t sum_len);
void topics_bump(const char *topic);
void topics_decay_all(void);
const char *topics_get_prompt(void);
const char *topics_get_line(void);
int  topics_search(const char *query, char *result_buf, size_t result_len,
                   int *found_out);

/* ── Events (events.c) ──────────────────────────────────────────── */
int events_store(const char *event_id, const char *content);

/* ── Episodes (episodes_file.c) ─────────────────────────────────── */
int ep_store(const char *episode_id, const char *summary, const char *event_id);
int ep_search(const char *query, char *result_buf, size_t result_len);

/* ── Log store (log_store.c) ─────────────────────────────────────── */
int log_store_append(const char *session_id,
                     const char *question, const char *answer);
int log_store_search(const char *query, char *result_buf, size_t result_len);
int log_store_read_by_ts(const char *ts_pattern,
                         char *result_buf, size_t result_len);

/* ── Semantic (semantic.c) ───────────────────────────────────────── */
int semantic_store(const char *knowledge, const char *tags);
int semantic_list(char *result_buf, size_t result_len);
int semantic_search(const char *query, char *result_buf, size_t result_len);

/* ── Segmentation (segment.c) ────────────────────────────────────── */
int  segment_feed(const char *question, const char *answer);
int  segment_detect_shift(const char *new_question);
const char *segment_current(void);
void segment_set_topic(const char *topic);
int  segment_flush(int importance);
void segment_init(void);

/* ── Directives (directives.c) ───────────────────────────────────── */
int directives_handle_archive(const char *text);
int directives_consolidate(void);

#endif /* BUSINESS_ARCHIVE_INTERNAL_H */
