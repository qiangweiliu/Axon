/*
 * topics_file.c — L1 topics.md file I/O (Execution Layer)
 *
 * Pure execution: reads/writes the pipe-delimited topics file.
 * On load, calls strength.h to recalculate scores.
 * No business logic beyond file format parsing.
 */

#include "agent_framework.h"
#include "os_api.h"
#include "topics_file.h"
#include "strength.h"
#include <string.h>
#include <stdlib.h>

#define LOAD_BUF 8192

int topics_file_load(archive_topic_t *topics, int *count_out)
{
    if (!topics || !count_out) return -1;
    *count_out = 0;

    os_file_handle_t fh = os_file_open(ARC_TOPICS_PATH, "r");
    if (!fh) return 0;  /* file doesn't exist yet */

    char buf[LOAD_BUF];
    size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
    os_file_close(fh);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *p = buf;
    int count = 0;
    while (*p && count < ARC_TOPICS_MAX) {
        char *nl = strchr(p, '\n');
        if (!nl) nl = p + os_strlen(p);

        archive_topic_t *t = &topics[count];
        os_memset(t, 0, sizeof(*t));

        char *parts[9];
        int nparts = 0;
        char *cur = p;
        while (cur < nl && nparts < 9) {
            parts[nparts++] = cur;
            char *pipe = strchr(cur, '|');
            if (!pipe) break;
            *pipe = '\0';
            cur = pipe + 1;
        }
        if (nparts >= 1) os_strncpy(t->topic, parts[0], sizeof(t->topic) - 1);
        if (nparts >= 2) t->score = atoi(parts[1]);
        if (nparts >= 3) t->importance = atoi(parts[2]);
        if (nparts >= 4) t->recall_count = atoi(parts[3]);
        if (nparts >= 5) t->last_access = (uint64_t)atoll(parts[4]);
        if (nparts >= 6) t->created = (uint64_t)atoll(parts[5]);
        if (nparts >= 7) os_strncpy(t->summary, parts[6], sizeof(t->summary) - 1);
        if (nparts >= 8) os_strncpy(t->event_id, parts[7], sizeof(t->event_id) - 1);
        if (nparts >= 9) os_strncpy(t->episode_id, parts[8], sizeof(t->episode_id) - 1);

        /* Recalc score — may have changed since last save */
        t->score = strength_calc(t);

        count++;
        p = *nl ? nl + 1 : nl;
    }
    *count_out = count;
    return 0;
}

int topics_file_save(const archive_topic_t *topics, int count)
{
    os_file_handle_t fh = os_file_open(ARC_TOPICS_PATH, "w");
    if (!fh) return -1;

    for (int i = 0; i < count; i++) {
        const archive_topic_t *t = &topics[i];
        char line[1024];
        os_snprintf(line, sizeof(line), "%s|%d|%d|%d|%llu|%llu|%s|%s|%s\n",
                    t->topic, t->score, t->importance, t->recall_count,
                    (unsigned long long)t->last_access,
                    (unsigned long long)t->created,
                    t->summary, t->event_id, t->episode_id);
        os_file_write(fh, line, os_strlen(line));
    }
    os_file_close(fh);
    return 0;
}
