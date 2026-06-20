/*
 * semantic.c — L4 semantic knowledge (data/memory/l4/semantic.tsv)
 *
 * Execution Layer. Simple TSV: id\tknowledge\ttags
 * Each store() appends; search() scans linearly.
 * List() returns all entries.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "archive.h"
#include <string.h>
#include <time.h>

#define SEM_PATH "data/memory/l4/semantic.tsv"
#define LINE_MAX 8192

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int semantic_store(const char *knowledge, const char *tags)
{
    if (!knowledge) return -1;

    os_file_handle_t fh = os_file_open(SEM_PATH, "a");
    if (!fh) return -1;

    char line[LINE_MAX];
    int n = os_snprintf(line, sizeof(line), "%llu\t%s\t%s\n",
                        (unsigned long long)now_ms(),
                        knowledge ? knowledge : "",
                        tags ? tags : "");
    if (n > 0) os_file_write(fh, line, (size_t)n);
    os_file_close(fh);
    return 0;
}

static int scan_tsv(const char *query, char *result_buf, size_t result_len)
{
    if (!result_buf) return -1;
    size_t pos = os_strlen(result_buf);
    int found = 0;

    os_file_handle_t fh = os_file_open(SEM_PATH, "r");
    if (!fh) return 0;

    char buf[32768];
    size_t n = os_file_read(fh, buf, sizeof(buf) - 1);
    os_file_close(fh);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *p = buf;
    while (*p && pos < result_len - 200) {
        char *nl = strchr(p, '\n');
        if (!nl) nl = p + os_strlen(p);
        size_t llen = (size_t)(nl - p);

        int match = 0;
        if (!query || !*query) {
            match = 1; /* list all */
        } else if (strstr(p, query)) {
            match = 1;
        }

        if (match) {
            char *tab1 = strchr(p, '\t');
            char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
            const char *knowledge = tab1 ? tab1 + 1 : p;
            const char *tags = tab2 ? tab2 + 1 : "";

            char kbuf[512];
            size_t klen = tab2 ? (size_t)(tab2 - tab1 - 1) : (tab1 ? llen - (size_t)(tab1 - p) - 1 : llen);
            if (klen > sizeof(kbuf) - 1) klen = sizeof(kbuf) - 1;
            os_memcpy(kbuf, knowledge, klen);
            kbuf[klen] = '\0';

            pos += os_snprintf(result_buf + pos, result_len - pos,
                "  · %s", kbuf);
            size_t tlen = tab2 ? llen - (size_t)(tab2 - p) - 1 : 0;
            if (tlen > 0 && tlen < 128) {
                char tbuf[128];
                os_memcpy(tbuf, tags, tlen);
                tbuf[tlen] = '\0';
                /* Trim trailing newline */
                while (tlen > 0 && (tbuf[tlen-1] == '\n' || tbuf[tlen-1] == '\r')) tbuf[--tlen] = '\0';
                if (tlen > 0) pos += os_snprintf(result_buf + pos, result_len - pos,
                    " [%s]", tbuf);
            }
            pos += os_snprintf(result_buf + pos, result_len - pos, "\n");
            found++;
        }
        p = *nl ? nl + 1 : nl;
    }
    return found;
}

int semantic_list(char *result_buf, size_t result_len)
{
    if (!result_buf || result_len < 1) return -1;
    size_t pos = 0;
    pos += os_snprintf(result_buf, result_len, "== Semantic Knowledge ==\n");
    int found = scan_tsv(NULL, result_buf, result_len);
    if (found == 0)
        pos += os_snprintf(result_buf + pos, result_len - pos, "  (no semantic knowledge yet)\n");
    return found;
}

int semantic_search(const char *query, char *result_buf, size_t result_len)
{
    if (!query || !result_buf) return -1;
    return scan_tsv(query, result_buf, result_len);
}
