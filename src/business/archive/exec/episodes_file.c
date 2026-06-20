/*
 * episodes_file.c — L2 episode storage (data/memory/l2/episodes.tsv)
 *
 * Execution Layer. Simple TSV: episode_id\tsummary\tevent_id
 * Each store() appends; search() scans linearly.
 */

#include "agent_framework.h"
#include "os_api.h"
#include <string.h>

#define EP_PATH "data/memory/l2/episodes.tsv"
#define LINE_MAX 4096

int ep_store(const char *episode_id, const char *summary, const char *event_id)
{
    os_file_handle_t fh = os_file_open(EP_PATH, "a");
    if (!fh) return -1;

    char line[LINE_MAX];
    int n = os_snprintf(line, sizeof(line), "%s\t%s\t%s\n",
                        episode_id ? episode_id : "",
                        summary ? summary : "",
                        event_id ? event_id : "");
    if (n > 0) os_file_write(fh, line, (size_t)n);
    os_file_close(fh);
    return 0;
}

int ep_search(const char *query, char *result_buf, size_t result_len)
{
    if (!query || !result_buf) return -1;
    size_t pos = os_strlen(result_buf);
    int found = 0;

    os_file_handle_t fh = os_file_open(EP_PATH, "r");
    if (!fh) return 0;

    char buf[16384];
    size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
    os_file_close(fh);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *p = buf;
    while (*p && pos < result_len - 200) {
        char *nl = strchr(p, '\n');
        if (!nl) nl = p + os_strlen(p);
        size_t llen = (size_t)(nl - p);

        if (strstr(p, query)) {
            /* Parse: episode_id\tsummary\tevent_id */
            char *tab1 = strchr(p, '\t');
            char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
            const char *ep_id = p;
            const char *summary = tab1 ? tab1 + 1 : "";
            const char *ev_id = tab2 ? tab2 + 1 : "";

            char ep_display[64], sum_display[256];
            size_t epl = tab1 ? (size_t)(tab1 - p) : llen;
            if (epl > sizeof(ep_display) - 1) epl = sizeof(ep_display) - 1;
            os_memcpy(ep_display, ep_id, epl);
            ep_display[epl] = '\0';

            size_t suml = tab2 ? (size_t)(tab2 - tab1 - 1) : 0;
            if (suml > sizeof(sum_display) - 1) suml = sizeof(sum_display) - 1;
            os_memcpy(sum_display, summary, suml);
            sum_display[suml] = '\0';

            pos += os_snprintf(result_buf + pos, result_len - pos,
                "  EPISODE %s: %s [event:%s]\n", ep_display, sum_display, ev_id);
            found++;
        }
        p = *nl ? nl + 1 : nl;
    }
    return found;
}
