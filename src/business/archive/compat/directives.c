/*
 * directives.c — LLM directive parsing for [ARCHIVE:] [RECALL:] [SEMANTIC:]
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "archive.h"
#include "archive_internal.h"
#include <string.h>
#include <stdlib.h>

/* ── [ARCHIVE:] parser ───────────────────────────────────────────── */

int directives_handle_archive(const char *text)
{
    if (!g_arc || !text) return -1;

    char topic[128] = "", episode[512] = "", tags[256] = "";
    int importance = ARC_IMP_MEDIUM;
    int detail_full = 0;

    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '|') p++;
        if (!*p) break;
        const char *eq = strchr(p, '=');
        if (!eq) break;

        size_t klen = (size_t)(eq - p);
        const char *val_start = eq + 1;
        const char *val_end = strchr(val_start, '|');
        if (!val_end) val_end = p + os_strlen(p);
        size_t vlen = (size_t)(val_end - val_start);

#define SET_FIELD(field, max) do { \
    if (vlen >= max) vlen = max - 1; \
    os_memcpy(field, val_start, vlen); \
    field[vlen] = '\0'; \
} while(0)

        if (klen == 5 && os_strncmp(p, "topic", 5) == 0)
            SET_FIELD(topic, sizeof(topic));
        else if (klen == 7 && os_strncmp(p, "episode", 7) == 0)
            SET_FIELD(episode, sizeof(episode));
        else if (klen == 4 && os_strncmp(p, "tags", 4) == 0)
            SET_FIELD(tags, sizeof(tags));
        else if (klen == 10 && os_strncmp(p, "importance", 10) == 0) {
            if (vlen >= 1) {
                char c = val_start[0] | 32; /* lowercase */
                if (c == 'l') importance = ARC_IMP_LOW;
                else if (c == 'm') importance = ARC_IMP_MEDIUM;
                else if (c == 'h') importance = ARC_IMP_HIGH;
                else if (c == 'f') importance = ARC_IMP_FLASH;
            }
        } else if (klen == 6 && os_strncmp(p, "detail", 6) == 0) {
            if (vlen >= 4 && (val_start[0] | 32) == 'f')
                detail_full = 1;
        }
        p = val_end;
    }

    if (!topic[0]) return -1;

    /* Generate chain IDs */
    char event_id[64], episode_id[64];
    uint64_t now = arc_now_ms();
    os_snprintf(event_id, sizeof(event_id), "%llu-%s",
                (unsigned long long)now, topic);
    os_snprintf(episode_id, sizeof(episode_id), "ep-%llu-%s",
                (unsigned long long)now, topic);
    for (char *c = event_id; *c; c++)
        if (*c == ' ' || *c == '/') *c = '-';
    for (char *c = episode_id; *c; c++)
        if (*c == ' ' || *c == '/') *c = '-';

    /* L1: Topic index */
    archive_add_topic(topic, importance, episode, event_id, episode_id);

    /* L2: Episode summary in memory.db */
    archive_store_episode(episode_id, episode, event_id);

    /* L3: Event detail */
    if (detail_full && episode[0])
        archive_store_detail(event_id, episode);

    LOG_INFO("Archive: topic='%s' ep='%s' ev='%s'", topic, episode_id, event_id);
    return 0;
}

/* ── [SEMANTIC:] parser — pure compat, no internal deps ────────── */

int archive_handle_semantic(const char *text)
{
    if (!text) return -1;

    /* Parse: knowledge=<value> | tags=<value> */
    const char *kn = strstr(text, "knowledge=");
    const char *tg = strstr(text, "tags=");

    char knowledge[1024] = "", tag_str[256] = "";

    if (kn) {
        kn += 10; /* skip "knowledge=" */
        int ki = 0;
        while (kn[ki] && kn[ki] != '|' && ki < (int)sizeof(knowledge) - 1) {
            knowledge[ki] = kn[ki]; ki++;
        }
        knowledge[ki] = '\0';
        while (ki > 0 && knowledge[ki - 1] == ' ') knowledge[--ki] = '\0';
    }

    if (tg) {
        tg += 5; /* skip "tags=" */
        int ti = 0;
        while (tg[ti] && tg[ti] != '|' && ti < (int)sizeof(tag_str) - 1) {
            tag_str[ti] = tg[ti]; ti++;
        }
        tag_str[ti] = '\0';
    }

    if (!knowledge[0]) {
        /* No knowledge= key; use raw text */
        os_strncpy(knowledge, text, sizeof(knowledge) - 1);
    }

    archive_semantic_store(knowledge, tag_str);
    LOG_INFO("Semantic directive: stored '%s'", knowledge);
    return 0;
}

/* ── Consolidation ────────────────────────────────────────────────── */

int directives_consolidate(void)
{
    if (!g_arc) return -1;
    int created = 0;

    for (int i = 0; i < g_arc->topic_count; i++) {
        archive_topic_t *t = &g_arc->topics[i];
        if (t->score < ARC_SCORE_MID) continue;
        if (t->summary[0] && os_strlen(t->summary) > 30) {
            char buf[768];
            os_snprintf(buf, sizeof(buf), "Event: %s — %s", t->topic, t->summary);
            archive_semantic_store(buf, "consolidated,auto");
            created++;
        }
    }
    if (created > 0)
        LOG_INFO("Consolidation: created %d semantic entries", created);
    return created;
}
