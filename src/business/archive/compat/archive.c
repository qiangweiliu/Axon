/*
 * archive.c — Compatibility Layer (External Interface → Core + Execution)
 *
 * Maps the stable archive.h public API to:
 *   Core Layer (strength.c, topics.c, segment.c, keywords.c)
 *   Execution Layer (topics_file.c, events_file.c, log_file.c, semantic_mem.c)
 *
 * If Execution layer changes (e.g., topics.md → SQLite),
 * only this file changes — archive.h stays the same.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "archive.h"
#include "archive_internal.h"
#include "keywords.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

/* ── Global context (shared with all sub-modules via internal.h) ─── */
archive_ctx_t *g_arc = NULL;

/* ── Time helpers ────────────────────────────────────────────────── */

uint64_t arc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int arc_days_since(uint64_t timestamp_ms)
{
    uint64_t now = arc_now_ms();
    if (now <= timestamp_ms) return 0;
    return (int)((now - timestamp_ms) / 86400000ULL);
}

int arc_calc_score(const archive_topic_t *t)
{
    int days = arc_days_since(t->last_access);
    int raw = (t->importance * W_IMPORTANCE) / 100
            + (days < 30 ? (100 - days * 3) * W_RECENCY / 100 : 0)
            + t->recall_count * W_FREQUENCY
            - days * W_DECAY;
    if (raw < 0) raw = 0;
    if (raw > 100) raw = 100;
    return raw;
}

/* ── Init ────────────────────────────────────────────────────────── */

int archive_init(void)
{
    if (g_arc) return 0;
    g_arc = (archive_ctx_t *)os_calloc(1, sizeof(archive_ctx_t));
    if (!g_arc) return -1;

    os_dir_create("data");
    os_dir_create("data/memory");
    os_dir_create("data/memory/l0");
    os_dir_create("data/memory/l1");
    os_dir_create("data/memory/l2");
    os_dir_create("data/memory/l3");
    os_dir_create("data/memory/l3/events");
    os_dir_create("data/memory/l4");
    os_dir_create("data/memory/l5");
    os_dir_create("data/memory/l5/archive");

    topics_init();
    segment_init();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Public API — each function delegates to the appropriate sub-module
 * ═══════════════════════════════════════════════════════════════════ */

/* L1 topics */
int archive_add_topic(const char *topic, int importance,
                      const char *summary, const char *event_id,
                      const char *episode_id)
    { return topics_add(topic, importance, summary, event_id, episode_id); }

int archive_find_topic(const char *topic, char *summary, size_t sum_len)
    { return topics_find(topic, summary, sum_len); }

const char *archive_get_topics_prompt(void)
    { return topics_get_prompt(); }

const char *archive_topics_line(void)
    { return topics_get_line(); }

void archive_bump_recall(const char *topic)
    { topics_bump(topic); }

void archive_decay_all(void)
    { topics_decay_all(); }

/* L3 events */
int archive_store_detail(const char *event_id, const char *content)
    { return events_store(event_id, content); }

int archive_store_episode(const char *episode_id,
                          const char *summary, const char *event_id)
{
    return ep_store(episode_id, summary, event_id);
}

/* L5 archive log */
int archive_append_log(const char *session_id,
                       const char *question, const char *answer)
    { return log_store_append(session_id, question, answer); }

/* L4 semantic */
int archive_semantic_store(const char *knowledge, const char *tags)
    { return semantic_store(knowledge, tags); }

int archive_semantic_list(char *result_buf, size_t result_len)
    { return semantic_list(result_buf, result_len); }

/* RECALL — hybrid search (L1 + memory.db) */
int archive_recall(const char *query, char *result_buf, size_t result_len)
{
    if (!query || !result_buf || result_len < 1) return -1;
    int found = 0;
    size_t pos = 0;
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "== Recall results for '%s' ==\n", query);

    topics_search(query, result_buf, result_len, &found);
    semantic_search(query, result_buf, result_len);

    /* L2: Search episodes file */
    found += ep_search(query, result_buf, result_len);

    /* L5: Search raw archive logs */
    found += log_store_search(query, result_buf, result_len);

    if (found == 0)
        pos += os_snprintf(result_buf + pos, result_len - pos, "  (nothing found)\n");
    return found;
}

/* ── Chain search ────────────────────────────────────────────────── */

int archive_chain(const char *query, int depth,
                  char *result_buf, size_t result_len)
{
    if (!query || !result_buf || result_len < 1) return -1;
    size_t pos = 0;
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "== Chain search '%s' (depth=%d) ==\n", query, depth);
    /* Always search L1 */
    int found = 0;
    topics_search(query, result_buf, result_len, &found);

    if (depth >= ARC_DEPTH_L2) {
        /* L2: For each matched topic, load episode from episode_id */
        for (int i = 0; i < g_arc->topic_count && pos < result_len - 100; i++) {
            if (g_arc->topics[i].episode_id[0] &&
                (strstr(g_arc->topics[i].topic, query) ||
                 strstr(g_arc->topics[i].summary, query))) {
                char ebuf[1024] = "";
                /* Search by episode_id in the episodes file */
                ep_search(g_arc->topics[i].episode_id, result_buf, result_len);
                found++;
            }
        }
        /* Also search episodes file directly for query */
        found += ep_search(query, result_buf, result_len);
    }

    if (depth >= ARC_DEPTH_L3) {
        /* L3: For matched topics (via event_id) or matched episodes (via event_id in content) */
        for (int i = 0; i < g_arc->topic_count && pos < result_len - 100; i++) {
            if (g_arc->topics[i].event_id[0] &&
                (strstr(g_arc->topics[i].topic, query) ||
                 strstr(g_arc->topics[i].summary, query))) {
                char path[512];
                os_snprintf(path, sizeof(path), "%s/%s.json",
                            ARC_EVENTS_DIR, g_arc->topics[i].event_id);
                os_file_handle_t fh = os_file_open(path, "r");
                if (fh) {
                    char ebuf[4096];
                    size_t rn = os_file_read(fh, ebuf, sizeof(ebuf) - 1);
                    os_file_close(fh);
                    if (rn > 0) {
                        ebuf[rn] = '\0';
                        pos += os_snprintf(result_buf + pos, result_len - pos,
                            "  EVENT %s:\n    %s\n",
                            g_arc->topics[i].event_id, ebuf);
                        found++;
                    }
                }
            }
        }
    }

    if (depth >= ARC_DEPTH_L4) {
        found += semantic_search(query, result_buf, result_len);
    }

    if (depth >= ARC_DEPTH_L5) {
        found += log_store_search(query, result_buf, result_len);
    }

    if (found == 0)
        pos += os_snprintf(result_buf + pos, result_len - pos, "  (nothing found at any depth)\n");
    return found;
}

/* LLM directives */
int archive_handle_directive(const char *text)
    { return directives_handle_archive(text); }

int archive_consolidate(void)
    { return directives_consolidate(); }

/* Event segmentation */
int archive_feed_turn(const char *q, const char *a)
    { return segment_feed(q, a); }

int archive_detect_topic_shift(const char *q)
    { return segment_detect_shift(q); }

const char *archive_current_segment(void)
    { return segment_current(); }

void archive_set_segment_topic(const char *t)
    { segment_set_topic(t); }

int archive_flush_segment(int importance)
    { return segment_flush(importance); }

/* ── Auto recall ─────────────────────────────────────────────────── */

static int has_recall_trigger(const char *q)
{
    const char *triggers[] = {
        "上次", "之前", "原来", "刚才", "以前",
        "remember", "again", "before", "previous", "earlier",
        "that", "what about", "you said",
        NULL
    };
    for (int i = 0; triggers[i]; i++) {
        if (strstr(q, triggers[i])) return 1;
    }
    return 0;
}

int archive_auto_recall(const char *question, char *result_buf, size_t result_len)
{
    if (!g_arc || !question || !result_buf || result_len < 1) return 0;

    size_t qlen = os_strlen(question);
    int has_trigger = has_recall_trigger(question);
    int keywords = 0;
    {
        char kw[256];
        kw_extract(question, kw, sizeof(kw));
        if (kw[0]) {
            char *p = kw;
            while (*p) { if (*p == ' ') keywords++; p++; }
            keywords++; /* last token */
        }
    }

    /* Scan L1 for matching topics */
    int best_idx = -1, best_score = 0;
    char kw_buf[256];
    kw_extract(question, kw_buf, sizeof(kw_buf));

    for (int i = 0; i < g_arc->topic_count; i++) {
        archive_topic_t *t = &g_arc->topics[i];
        /* Extract keywords from topic name + summary */
        char text[320];
        os_snprintf(text, sizeof(text), "%s %s", t->topic, t->summary);
        char tk[256];
        kw_extract(text, tk, sizeof(tk));
        double overlap = kw_overlap(kw_buf, tk);
        if (overlap > 0.15 && t->score > best_score) {
            best_score = t->score;
            best_idx = i;
        }
    }

    if (best_idx < 0) return 0; /* nothing relevant */

    /* Determine depth */
    int depth = ARC_DEPTH_L1;
    if (has_trigger) {
        depth = ARC_DEPTH_L3;     /* "上次说" → 直接到事件 */
    } else if (best_score < 45) {
        depth = ARC_DEPTH_L2;     /* 模糊记忆 → 需要摘要 */
    }
    if (keywords >= 3 && depth < ARC_DEPTH_L3)
        depth = ARC_DEPTH_L3;     /* 多关键词 → 更可能指具体事件 */
    if (qlen > 40 && depth < ARC_DEPTH_L4)
        depth = ARC_DEPTH_L4;     /* 长问题 → 查语义层 */
    if (depth > g_arc->topic_count) depth = g_arc->topic_count;

    /* Walk the chain */
    size_t pos = 0;
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "===== RECALLED =====\n");

    archive_topic_t *t = &g_arc->topics[best_idx];
    pos += os_snprintf(result_buf + pos, result_len - pos,
        "L1: %s(%d): %s\n", t->topic, t->score, t->summary);
    topics_bump(t->topic); /* strengthen on recall */

    if (depth >= ARC_DEPTH_L2 && t->episode_id[0]) {
        ep_search(t->episode_id, result_buf, result_len);
    }

    if (depth >= ARC_DEPTH_L3 && t->event_id[0]) {
        char path[512];
        os_snprintf(path, sizeof(path), "%s/%s.json",
                    ARC_EVENTS_DIR, t->event_id);
        os_file_handle_t fh = os_file_open(path, "r");
        if (fh) {
            char ebuf[4096];
            size_t rn = os_file_read(fh, ebuf, sizeof(ebuf) - 1);
            os_file_close(fh);
            if (rn > 0) {
                ebuf[rn] = '\0';
                /* Truncate long events */
                if (rn > 500) {
                    os_strncpy(ebuf + 500, "\n... (truncated)", sizeof(ebuf) - 500 - 1);
                }
                pos += os_snprintf(result_buf + pos, result_len - pos,
                    "L3:\n  %s\n", ebuf);
            }
        }
    }

    if (depth >= ARC_DEPTH_L4) {
        semantic_search(t->topic, result_buf, result_len);
    }

    if (depth >= ARC_DEPTH_L5 && t->event_id[0]) {
        log_store_search(t->event_id, result_buf, result_len);
    }

    pos += os_snprintf(result_buf + pos, result_len - pos,
        "=====\n");
    return 1;
}

/* ── Admin: clear all memories ───────────────────────────────────── */

#include <unistd.h>
#include <dirent.h>

static void rm_recursive(const char *dir)
{
    DIR *dh = opendir(dir);
    if (!dh) return;
    struct dirent *entry;
    while ((entry = readdir(dh)) != NULL) {
        if (os_strcmp(entry->d_name, ".") == 0 ||
            os_strcmp(entry->d_name, "..") == 0)
            continue;
        char full[512];
        os_snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        DIR *sub = opendir(full);
        if (sub) {
            closedir(sub);
            rm_recursive(full);
            rmdir(full);
        } else {
            unlink(full);
        }
    }
    closedir(dh);
}

int archive_clear_all(void)
{
    /* L0: Clear bounded memory files */
    os_file_handle_t fh;

    fh = os_file_open("data/memory/l0/working.md", "w");
    if (fh) os_file_close(fh);

    fh = os_file_open("data/memory/l0/profile.md", "w");
    if (fh) os_file_close(fh);

    /* L1: Clear topics index */
    fh = os_file_open(ARC_TOPICS_PATH, "w");
    if (fh) os_file_close(fh);
    if (g_arc) {
        g_arc->topic_count = 0;
        g_arc->topics_dirty = 0;
        g_arc->prompt_dirty = 1;
        g_arc->line_dirty = 1;
    }

    /* L2+L4: Clear memory.db */
    fh = os_file_open("data/memory.db", "w");
    if (fh) os_file_close(fh);

    /* L3: Clear events directory */
    rm_recursive(ARC_EVENTS_DIR);
    os_dir_create(ARC_EVENTS_DIR);

    /* L5: Clear archive directory */
    rm_recursive(ARC_ARCHIVE_DIR);
    os_dir_create(ARC_ARCHIVE_DIR);

    LOG_INFO("Archive: all memory cleared");
    return 0;
}

/* ── Module Registration ─────────────────────────────────────────── */

static int module_init(framework_module_t *mod)
{
    (void)mod;
    return archive_init();
}

static int module_start(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("Archive: ready (%d topics)", g_arc ? g_arc->topic_count : 0);
    int n = directives_consolidate();
    if (n > 0) LOG_INFO("Archive: consolidation created %d semantic entries", n);
    return 0;
}

framework_module_t archive_mod = {
    .name = "archive",
    .version = 0x00010000,
    .state = FRAMEWORK_STATE_UNLOADED,
    .init = module_init,
    .start = module_start,
    .loop = NULL,
    .stop = NULL,
    .deinit = NULL,
    .ctx = NULL,
    .id = 0,
    .next = NULL,
};
MODULE_REGISTER(archive_mod);
