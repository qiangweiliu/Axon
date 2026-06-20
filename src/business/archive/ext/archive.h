/*
 * archive.h — Layered memory archive (human-inspired memory model)
 *
 * Business layer (priority=360). Manages 6 memory tiers:
 *   L0: Working memory (existing memfile)
 *   L1: Topic index (memories/topics.md) — compact, always in prompt
 *   L2: Episode (data/memory.db type=episode) — strength-scored
 *   L3: Detail (data/events/{id}.json) — full event record
 *   L4: Semantic (data/memory.db type=semantic) — distilled knowledge
 *   L5: Archive (data/archive/) — raw conversation logs, never deleted
 *
 * Strength score formula for L2:
 *   score = W_imp * importance + W_rec * recency + W_freq * recall_count
 *           - W_decay * days_since
 */

#ifndef BUSINESS_ARCHIVE_H
#define BUSINESS_ARCHIVE_H

#include <stdint.h>
#include <stddef.h>

/* ── Score Weights ───────────────────────────────────────────────── */
#define W_IMPORTANCE     30   /* 0-100 → max 3000 */
#define W_RECENCY         4   /* 0-100 → max 400  */
#define W_FREQUENCY       4   /* per recall → cumulative */
#define W_DECAY            5   /* per day since event */

/* Importance levels (LLM-facing) */
#define ARC_IMP_LOW       20
#define ARC_IMP_MEDIUM    50
#define ARC_IMP_HIGH      80
#define ARC_IMP_FLASH    100   /* flashbulb: never pruned */

/* Thresholds */
#define ARC_SCORE_MIN     20    /* below this: L1 removed (forgotten) */
#define ARC_SCORE_MID     45    /* below this: L3 pruned */

/* Paths */
#define ARC_TOPICS_PATH   "data/memory/l1/topics.md"
#define ARC_EVENTS_DIR    "data/memory/l3/events"
#define ARC_ARCHIVE_DIR   "data/memory/l5/archive"

/* Max entries */
#define ARC_TOPICS_MAX    50
#define ARC_SUMMARY_MAX   256
#define ARC_NAME_MAX      64

/* ── Topic entry (L1) ────────────────────────────────────────────── */
typedef struct {
    char     topic[ARC_NAME_MAX];
    int      score;          /* current strength */
    int      importance;
    int      recall_count;
    uint64_t last_access;    /* timestamp ms */
    uint64_t created;        /* timestamp ms */
    char     summary[ARC_SUMMARY_MAX];
    char     event_id[64];   /* links to L3 events/{id}.json */
    char     episode_id[64]; /* links to L2 memory.db type=episode */
} archive_topic_t;

/* ── Public API ──────────────────────────────────────────────────── */

/* Init: ensure directories exist, load topics from disk */
int archive_init(void);

/* Handle [ARCHIVE: ...] directive from LLM.
   Format: [ARCHIVE: topic=<s> | episode=<s> | importance=<low|medium|high|flash> |
            tags=<a,b,c> | detail=<full|summary|auto>]
   Returns 0 on success, -1 on error. */
int archive_handle_directive(const char *directive_text);

/* Handle [SEMANTIC: ...] directive from LLM.
   Format: [SEMANTIC: knowledge=<fact> | tags=<cat>]
   The text between "[SEMANTIC:" and "]" is passed in.
   Parses knowledge= and tags= fields, stores via archive_semantic_store().
   Returns 0 on success, -1 on error. */
int archive_handle_semantic(const char *directive_text);

/* Add an entry to L1 topic index (with strength score) */
int archive_add_topic(const char *topic, int importance,
                      const char *summary, const char *event_id,
                      const char *episode_id);

/* Find a topic by name, returns score or -1 */
int archive_find_topic(const char *topic, char *summary, size_t sum_len);

/* Get the topics index string for system prompt injection.
   Returns a string like:
   "== Topics ==\n  topic1 (92): summary...\n  topic2 (45): ..."
   Caller must NOT free the returned pointer. */
const char *archive_get_topics_prompt(void);

/* Bump recall count and recalc score for a topic.
   Called when LLM mentions/references an archived topic. */
void archive_bump_recall(const char *topic);

/* Store a detailed event to L3 (data/events/{id}.json) */
int archive_store_detail(const char *event_id, const char *content);

/* Store an L2 episode summary to memory.db (type=episode).
 * episode_id: unique identifier (e.g. "ep-<ts>-<topic>").
 * summary: brief description.
 * event_id: links to L3 event detail.
 * Returns 0 on success. */
int archive_store_episode(const char *episode_id,
                          const char *summary, const char *event_id);

/* Append raw conversation to L5 archive */
int archive_append_log(const char *session_id,
                       const char *question, const char *answer);

/* Get a formatted topics line for the system prompt.
   Returns compact single line: "Topics: topic1(score) topic2(score)..." */
const char *archive_topics_line(void);

/* Periodically decay scores for all topics. Call once per session start. */
void archive_decay_all(void);

/* ── RECALL — chain search with depth ────────────────────────────── */

/* Depth constants for chain search */
#define ARC_DEPTH_L1      1   /* topic index only */
#define ARC_DEPTH_L2      2   /* + episode summary */
#define ARC_DEPTH_L3      3   /* + event detail */
#define ARC_DEPTH_L4      4   /* + semantic knowledge */
#define ARC_DEPTH_L5      5   /* + raw archive logs */

/* Search archived memories (L1 topics + L2 episodes + L4 semantic).
 * query: keyword or phrase to search for.
 * result_buf: caller-allocated buffer for formatted results.
 * result_len: size of result buffer.
 * Returns number of matches found. */
int archive_recall(const char *query, char *result_buf, size_t result_len);

/* Chain search: from L1 down to specified depth along reference links.
 * depth: ARC_DEPTH_L1 to ARC_DEPTH_L5.
 * Returns number of chain items found. */
int archive_chain(const char *query, int depth,
                  char *result_buf, size_t result_len);

/* ── SEMANTIC (L4) ────────────────────────────────────────────────── */

/* Store a piece of semantic knowledge.
 * knowledge: the statement/fact to remember (e.g. "User prefers pytest").
 * tags: comma-separated category tags (e.g. "coding,testing,preference").
 * Returns 0 on success, -1 on error. */
int archive_semantic_store(const char *knowledge, const char *tags);

/* List all stored semantic knowledge.
 * result_buf: caller-allocated buffer.
 * result_len: buffer size.
 * Returns number of semantic entries found. */
int archive_semantic_list(char *result_buf, size_t result_len);

/* ── CONSOLIDATION ────────────────────────────────────────────────── */

/* Run consolidation: extract candidate semantic knowledge from recent
 * high-score L1 topics and their L3 details.
 * This is an idempotent operation — call periodically or at session start.
 * Returns number of new semantic entries created. */
int archive_consolidate(void);

/* ── EVENT SEGMENTATION (conversation → events) ──────────────────── */

/* Maximum turns tracked per segment */
#define ARC_SEGMENT_TURNS_MAX 8
#define ARC_TURN_TEXT_MAX    256

/* Feed a conversation turn into the segment tracker.
 * question: the user's input for this turn.
 * answer: the LLM's response (can be NULL on first call).
 * Returns the current segment depth (number of turns in this segment). */
int archive_feed_turn(const char *question, const char *answer);

/* Detect whether a new question represents a topic shift from current segment.
 * Uses keyword overlap heuristic.
 * Returns 1 if shift detected, 0 if same topic. */
int archive_detect_topic_shift(const char *new_question);

/* Get current segment topic.
 * If no topic set yet, returns "conversation-<timestamp>". */
const char *archive_current_segment(void);

/* Flush current segment: auto-generate topic, store to L1/L3.
 * Called on session exit or explicit topic shift.
 * importance: use ARC_IMP_MEDIUM for auto-flush.
 * Returns 0 on success. */
int archive_flush_segment(int importance);

/* Set segment topic explicitly (called when LLM emits [ARCHIVE:]). */
void archive_set_segment_topic(const char *topic);

/* ── AUTO RECALL ─────────────────────────────────────────────────── */

/* Automatically search all memory tiers based on user question.
 * Detects keywords, scans L0-L5, returns best-effort recalled content
 * formatted for prompt injection.
 * Called every turn before BUILD_PROMPT().
 * Returns 0 if nothing recalled, 1 if something found. */
int archive_auto_recall(const char *question, char *result_buf, size_t result_len);

/* ── ADMIN ───────────────────────────────────────────────────────── */

/* Clear all memory storage across all tiers.
 * L0: memories/memory.md, memories/user.md
 * L1: memories/topics.md
 * L2+L4: data/memory.db (all entries)
 * L3: data/events/ (JSON files)
 * L5: data/archive/ (all logs)
 * After this, the agent starts with a blank slate.
 * Returns 0 on success. */
int archive_clear_all(void);

#endif /* BUSINESS_ARCHIVE_H */
