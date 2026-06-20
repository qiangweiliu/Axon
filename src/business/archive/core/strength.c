/*
 * strength.c — Pure memory strength score engine (Core Layer)
 *
 * No I/O. No os_* calls. Pure math.
 */

#include "strength.h"
#include <time.h>

/* ── Time helper (pure: wall-clock-free for unit tests) ──────────── */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int days_since(uint64_t ts)
{
    uint64_t now = now_ms();
    if (now <= ts) return 0;
    return (int)((now - ts) / 86400000ULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

int strength_calc(const archive_topic_t *t)
{
    int days = days_since(t->last_access);
    int raw = (t->importance * W_IMPORTANCE) / 100
            + (days < 30 ? (100 - days * 3) * W_RECENCY / 100 : 0)
            + t->recall_count * W_FREQUENCY
            - days * W_DECAY;
    if (raw < 0) raw = 0;
    if (raw > 100) raw = 100;
    return raw;
}

int strength_decay_all(archive_topic_t *topics, int count)
{
    int changed = 0;
    for (int i = 0; i < count; i++) {
        int old = topics[i].score;
        topics[i].score = strength_calc(&topics[i]);
        if (old != topics[i].score) changed++;
    }
    return changed;
}

int strength_evict_candidate(const archive_topic_t *topics, int count)
{
    int worst = -1;
    int worst_score = 999;
    for (int i = 0; i < count; i++) {
        if (topics[i].importance < ARC_IMP_FLASH &&
            topics[i].score < worst_score) {
            worst_score = topics[i].score;
            worst = i;
        }
    }
    return worst;
}
