/*
 * topics.c — L1 topic index management (Core Layer)
 *
 * Pure logic: topic CRUD, cache management, prompt formatting.
 * No I/O — delegates file operations to topics_file.h (Execution Layer).
 * Score math delegated to strength.h (Core Layer).
 */

#include "agent_framework.h"
#include "os_api.h"
#include "archive_internal.h"
#include "strength.h"
#include "topics_file.h"
#include <string.h>

/* ── Init / Load ─────────────────────────────────────────────────── */

void topics_init(void)
{
    if (!g_arc) return;
    int loaded = 0;
    topics_file_load(g_arc->topics, &loaded);
    g_arc->topic_count = loaded;
    g_arc->topics_dirty = 0;
    g_arc->prompt_dirty = 1;
    g_arc->line_dirty = 1;
    LOG_INFO("Topics: loaded %d topics", loaded);
}

void topics_mark_dirty(void)
{
    if (!g_arc) return;
    g_arc->topics_dirty = 1;
    g_arc->prompt_dirty = 1;
    g_arc->line_dirty = 1;
}

int topics_save(void)
{
    if (!g_arc || !g_arc->topics_dirty) return 0;
    if (topics_file_save(g_arc->topics, g_arc->topic_count) == 0) {
        g_arc->topics_dirty = 0;
        return 0;
    }
    return -1;
}

/* ── Core: Add/Update/Evict ──────────────────────────────────────── */

int topics_add(const char *topic, int importance,
               const char *summary, const char *event_id,
               const char *episode_id)
{
    if (!g_arc || !topic || !*topic) return -1;
    int i;

    /* Update existing */
    for (i = 0; i < g_arc->topic_count; i++) {
        if (os_strcmp(g_arc->topics[i].topic, topic) == 0) {
            archive_topic_t *t = &g_arc->topics[i];
            t->importance = importance > t->importance ? importance : t->importance;
            t->recall_count++;
            t->last_access = arc_now_ms();
            if (summary && *summary) os_strncpy(t->summary, summary, sizeof(t->summary) - 1);
            if (event_id && *event_id) os_strncpy(t->event_id, event_id, sizeof(t->event_id) - 1);
            if (episode_id && *episode_id) os_strncpy(t->episode_id, episode_id, sizeof(t->episode_id) - 1);
            t->score = strength_calc(t);
            topics_mark_dirty();
            topics_save();
            return i;
        }
    }

    /* Evict if full */
    if (g_arc->topic_count >= ARC_TOPICS_MAX) {
        int worst = strength_evict_candidate(g_arc->topics, g_arc->topic_count);
        if (worst >= 0) i = worst;
        else return -1;
    } else {
        i = g_arc->topic_count;
        g_arc->topic_count++;
    }

    archive_topic_t *t = &g_arc->topics[i];
    os_memset(t, 0, sizeof(*t));
    os_strncpy(t->topic, topic, sizeof(t->topic) - 1);
    t->importance = importance;
    t->recall_count = 1;
    t->last_access = arc_now_ms();
    t->created = arc_now_ms();
    if (summary) os_strncpy(t->summary, summary, sizeof(t->summary) - 1);
    if (event_id) os_strncpy(t->event_id, event_id, sizeof(t->event_id) - 1);
    if (episode_id) os_strncpy(t->episode_id, episode_id, sizeof(t->episode_id) - 1);
    t->score = strength_calc(t);
    topics_mark_dirty();
    topics_save();
    return i;
}

/* ── Core: Query ─────────────────────────────────────────────────── */

int topics_find(const char *topic, char *summary, size_t sum_len)
{
    if (!g_arc || !topic) return -1;
    for (int i = 0; i < g_arc->topic_count; i++) {
        if (os_strcmp(g_arc->topics[i].topic, topic) == 0) {
            if (summary) {
                os_strncpy(summary, g_arc->topics[i].summary, sum_len - 1);
                summary[sum_len - 1] = '\0';
            }
            return g_arc->topics[i].score;
        }
    }
    return -1;
}

void topics_bump(const char *topic)
{
    if (!g_arc || !topic) return;
    for (int i = 0; i < g_arc->topic_count; i++) {
        if (os_strcmp(g_arc->topics[i].topic, topic) == 0) {
            g_arc->topics[i].recall_count++;
            g_arc->topics[i].last_access = arc_now_ms();
            g_arc->topics[i].score = strength_calc(&g_arc->topics[i]);
            topics_mark_dirty();
            topics_save();
            return;
        }
    }
}

void topics_decay_all(void)
{
    if (!g_arc) return;
    int changed = strength_decay_all(g_arc->topics, g_arc->topic_count);
    if (changed) { topics_mark_dirty(); topics_save(); }
}

/* ── Core: Prompt formatting ─────────────────────────────────────── */

const char *topics_get_prompt(void)
{
    if (!g_arc) return NULL;
    if (!g_arc->prompt_dirty && g_arc->topics_prompt[0])
        return g_arc->topics_prompt;

    int pos = 0;
    pos += os_snprintf(g_arc->topics_prompt, sizeof(g_arc->topics_prompt),
        "== Topics (recent memories, strength scores) ==\n");
    for (int i = 0; i < g_arc->topic_count && pos < (int)sizeof(g_arc->topics_prompt) - 4; i++) {
        archive_topic_t *t = &g_arc->topics[i];
        if (t->score < ARC_SCORE_MIN) continue; /* skip forgotten */
        int days = arc_days_since(t->created);
        pos += os_snprintf(g_arc->topics_prompt + pos, sizeof(g_arc->topics_prompt) - pos,
            "  %s (%d): %s [%dd ago]\n", t->topic, t->score, t->summary, days);
    }
    g_arc->prompt_dirty = 0;
    return g_arc->topics_prompt;
}

const char *topics_get_line(void)
{
    if (!g_arc) return NULL;
    if (!g_arc->line_dirty && g_arc->topics_line[0])
        return g_arc->topics_line;

    int pos = 0;
    pos += os_snprintf(g_arc->topics_line, sizeof(g_arc->topics_line), "Topics:");
    for (int i = 0; i < g_arc->topic_count && pos < (int)sizeof(g_arc->topics_line) - 20; i++) {
        if (g_arc->topics[i].score < ARC_SCORE_MIN) continue; /* skip forgotten */
        pos += os_snprintf(g_arc->topics_line + pos, sizeof(g_arc->topics_line) - pos,
            " %s(%d)", g_arc->topics[i].topic, g_arc->topics[i].score);
    }
    pos += os_snprintf(g_arc->topics_line + pos, sizeof(g_arc->topics_line) - pos, "\n");
    g_arc->line_dirty = 0;
    return g_arc->topics_line;
}

int topics_search(const char *query, char *result_buf, size_t result_len,
                  int *found_out)
{
    if (!g_arc || !query || !result_buf) return -1;
    int found = 0;
    size_t pos = os_strlen(result_buf);

    for (int i = 0; i < g_arc->topic_count && pos < result_len - 100; i++) {
        archive_topic_t *t = &g_arc->topics[i];
        if (strstr(t->topic, query) || strstr(t->summary, query)) {
            pos += os_snprintf(result_buf + pos, result_len - pos,
                "  TOPIC %s (%d): %s\n", t->topic, t->score, t->summary);
            found++;
            topics_bump(t->topic);
        }
    }
    if (found_out) *found_out = found;
    return 0;
}
