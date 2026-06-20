/*
 * segment.c — Event segmentation (conversation → events)
 *
 * Tracks conversation turns, detects topic shifts via keyword overlap,
 * auto-flushes segments to L1+L3 storage.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "archive_internal.h"
#include "keywords.h"
#include <string.h>

/* ── Feed turn ───────────────────────────────────────────────────── */

int segment_feed(const char *question, const char *answer)
{
    if (!g_arc || !question) return -1;

    if (g_arc->seg_depth < ARC_SEGMENT_TURNS_MAX) {
        os_strncpy(g_arc->seg_turns_q[g_arc->seg_depth], question, ARC_TURN_TEXT_MAX - 1);
        if (answer) os_strncpy(g_arc->seg_turns_a[g_arc->seg_depth], answer, ARC_TURN_TEXT_MAX - 1);
        g_arc->seg_depth++;
    } else {
        for (int i = 1; i < ARC_SEGMENT_TURNS_MAX; i++) {
            os_memcpy(g_arc->seg_turns_q[i-1], g_arc->seg_turns_q[i], ARC_TURN_TEXT_MAX);
            os_memcpy(g_arc->seg_turns_a[i-1], g_arc->seg_turns_a[i], ARC_TURN_TEXT_MAX);
        }
        os_strncpy(g_arc->seg_turns_q[ARC_SEGMENT_TURNS_MAX - 1], question, ARC_TURN_TEXT_MAX - 1);
        if (answer) os_strncpy(g_arc->seg_turns_a[ARC_SEGMENT_TURNS_MAX - 1], answer, ARC_TURN_TEXT_MAX - 1);
    }
    return g_arc->seg_depth;
}

int segment_detect_shift(const char *new_question)
{
    if (!g_arc || !new_question || !g_arc->seg_initialized) return 0;
    if (g_arc->seg_depth < 1) return 0;

    char new_kw[256];
    kw_extract(new_question, new_kw, sizeof(new_kw));

    double max_overlap = 0.0;
    for (int i = 0; i < g_arc->seg_depth && i < ARC_SEGMENT_TURNS_MAX; i++) {
        char old_kw[256];
        kw_extract(g_arc->seg_turns_q[i], old_kw, sizeof(old_kw));
        double ov = kw_overlap(new_kw, old_kw);
        if (ov > max_overlap) max_overlap = ov;
    }
    return (max_overlap < 0.15) ? 1 : 0;
}

const char *segment_current(void)
{
    if (!g_arc) return "unknown";
    return g_arc->seg_topic;
}

void segment_set_topic(const char *topic)
{
    if (!g_arc || !topic) return;
    os_strncpy(g_arc->seg_topic, topic, sizeof(g_arc->seg_topic) - 1);
}

int segment_flush(int importance)
{
    if (!g_arc || g_arc->seg_depth == 0) return 0;

    char summary[1024];
    int pos = 0;
    for (int i = 0; i < g_arc->seg_depth && pos < 900; i++) {
        if (i > 0) pos += os_snprintf(summary + pos, sizeof(summary) - pos, " | ");
        pos += os_snprintf(summary + pos, sizeof(summary) - pos, "%s",
                           g_arc->seg_turns_q[i]);
    }

    char event_id[64], episode_id[64];
    os_snprintf(event_id, sizeof(event_id), "%llu-%s",
                (unsigned long long)g_arc->seg_start_ms, g_arc->seg_topic);
    os_snprintf(episode_id, sizeof(episode_id), "ep-%llu-%s",
                (unsigned long long)g_arc->seg_start_ms, g_arc->seg_topic);

    /* Store to L1 + L3 via archive.h public API */
    archive_add_topic(g_arc->seg_topic, importance, summary, event_id, episode_id);

    /* L2: Episode summary in memory.db */
    archive_store_episode(episode_id, summary, event_id);

    char detail[8192];
    int dp = 0;
    for (int i = 0; i < g_arc->seg_depth && dp < 8000; i++) {
        dp += os_snprintf(detail + dp, sizeof(detail) - dp,
            "Q: %s\nA: %s\n---\n",
            g_arc->seg_turns_q[i], g_arc->seg_turns_a[i]);
    }
    archive_store_detail(event_id, detail);

    LOG_INFO("Segment: flushed '%s' (%d turns, imp=%d)",
             g_arc->seg_topic, g_arc->seg_depth, importance);

    g_arc->seg_depth = 0;
    g_arc->seg_start_ms = arc_now_ms();
    os_snprintf(g_arc->seg_topic, sizeof(g_arc->seg_topic), "session-%llu",
                (unsigned long long)g_arc->seg_start_ms);
    return 0;
}

void segment_init(void)
{
    if (!g_arc) return;
    g_arc->seg_start_ms = arc_now_ms();
    os_snprintf(g_arc->seg_topic, sizeof(g_arc->seg_topic), "session-%llu",
                (unsigned long long)g_arc->seg_start_ms);
    g_arc->seg_depth = 0;
    g_arc->seg_initialized = 1;
}
