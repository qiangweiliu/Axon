/*
 * events.c — L3 event detail storage (data/events/{id}.json)
 */

#include "agent_framework.h"
#include "os_api.h"
#include "archive.h"
#include <string.h>

int events_store(const char *event_id, const char *content)
{
    if (!event_id || !content) return -1;
    char path[512];
    os_snprintf(path, sizeof(path), "%s/%s.json", ARC_EVENTS_DIR, event_id);
    os_file_handle_t fh = os_file_open(path, "w");
    if (!fh) return -1;
    os_file_write(fh, content, os_strlen(content));
    os_file_close(fh);
    return 0;
}
