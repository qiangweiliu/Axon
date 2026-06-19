/*
 * bus.c — Event bus: pub/sub with priority-ordered subscribers.
 *
 * Event type (uint32) maps to a list of subscribers.
 * Max: 1024 event types, 128 subscribers per type.
 * Event data is deep-copied (allocated via framework_alloc) so the
 * publisher can free its buffer immediately after publish().
 */

#include "framework_internal.h"
#include "os_api.h"

typedef struct {
    framework_event_handler_t handler;
    int priority;
    void *user_data;
} subscriber_t;

typedef struct {
    framework_event_type_t type;
    subscriber_t subscribers[MAX_SUBSCRIBERS];
    int subscriber_count;
} event_type_entry_t;

static event_type_entry_t g_event_table[MAX_EVENT_TYPES] = {{0}};
static os_mutex_handle_t g_bus_mutex = NULL;

int framework_bus_init(void)
{
    if (!g_bus_mutex) {
        g_bus_mutex = os_mutex_create();
        if (!g_bus_mutex) return -1;
    }
    return 0;
}

static event_type_entry_t *find_event_type(framework_event_type_t type)
{
    for (int i = 0; i < MAX_EVENT_TYPES; i++) {
        if (g_event_table[i].type == type || g_event_table[i].subscriber_count == 0) {
            if (g_event_table[i].type == type) {
                return &g_event_table[i];
            }
            g_event_table[i].type = type;
            g_event_table[i].subscriber_count = 0;
            return &g_event_table[i];
        }
    }
    return NULL;
}

static void sort_subscribers(event_type_entry_t *entry)
{
    for (int i = 1; i < entry->subscriber_count; i++) {
        subscriber_t tmp = entry->subscribers[i];
        int j = i - 1;
        while (j >= 0 && entry->subscribers[j].priority < tmp.priority) {
            entry->subscribers[j + 1] = entry->subscribers[j];
            j--;
        }
        entry->subscribers[j + 1] = tmp;
    }
}

int framework_event_subscribe(framework_event_type_t type,
                              framework_event_handler_t handler,
                              int priority,
                              void *user_data)
{
    if (!handler) return -1;

    os_mutex_lock(g_bus_mutex);

    event_type_entry_t *entry = find_event_type(type);
    if (!entry) {
        os_mutex_unlock(g_bus_mutex);
        return -1;
    }

    if (entry->subscriber_count >= MAX_SUBSCRIBERS) {
        os_mutex_unlock(g_bus_mutex);
        return -1;
    }

    subscriber_t *sub = &entry->subscribers[entry->subscriber_count++];
    sub->handler = handler;
    sub->priority = priority;
    sub->user_data = user_data;

    sort_subscribers(entry);

    os_mutex_unlock(g_bus_mutex);
    return 0;
}

int framework_event_unsubscribe(framework_event_type_t type,
                                framework_event_handler_t handler)
{
    if (!handler) return -1;

    os_mutex_lock(g_bus_mutex);

    for (int i = 0; i < MAX_EVENT_TYPES; i++) {
        if (g_event_table[i].type != type) continue;

        for (int j = 0; j < g_event_table[i].subscriber_count; j++) {
            if (g_event_table[i].subscribers[j].handler == handler) {
                for (int k = j; k < g_event_table[i].subscriber_count - 1; k++) {
                    g_event_table[i].subscribers[k] = g_event_table[i].subscribers[k + 1];
                }
                g_event_table[i].subscriber_count--;
                os_mutex_unlock(g_bus_mutex);
                return 0;
            }
        }
    }

    os_mutex_unlock(g_bus_mutex);
    return -1;
}

int framework_event_publish(framework_event_type_t type,
                            const void *data,
                            size_t data_size)
{
    os_mutex_lock(g_bus_mutex);

    event_type_entry_t *entry = NULL;
    for (int i = 0; i < MAX_EVENT_TYPES; i++) {
        if (g_event_table[i].type == type) {
            entry = &g_event_table[i];
            break;
        }
    }

    if (!entry) {
        os_mutex_unlock(g_bus_mutex);
        return 0; /* No subscribers — not an error */
    }

    /* Deep-copy data if needed */
    void *copy = NULL;
    if (data && data_size > 0) {
        copy = framework_alloc(data_size);
        if (copy) {
            os_memcpy(copy, data, data_size);
        }
    }

    /* Dispatch to subscribers in priority order */
    for (int i = 0; i < entry->subscriber_count; i++) {
        entry->subscribers[i].handler(type, copy, data_size,
                                       entry->subscribers[i].user_data);
    }

    /* Free copy */
    if (copy) {
        framework_free(copy);
    }

    os_mutex_unlock(g_bus_mutex);
    return 0;
}
