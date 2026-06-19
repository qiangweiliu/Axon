/*
 * framework_internal.h — Private data structures.
 */

#ifndef FRAMEWORK_INTERNAL_H
#define FRAMEWORK_INTERNAL_H

#include "agent_framework.h"

#define MAX_MODULES        128
#define MAX_SUBSCRIBERS    128
#define MAX_EVENT_TYPES    1024
#define ALLOC_TRACK_MAX    256

typedef struct {
    void *ptr;
    size_t size;
    const char *file;
    int line;
    int active;
} alloc_entry_t;

typedef struct {
    alloc_entry_t entries[ALLOC_TRACK_MAX];
    int count;
} alloc_tracker_t;

typedef struct {
    framework_module_t *list[MAX_MODULES];
    int count;
} framework_registry_t;

extern framework_registry_t g_registry;

void _fw_module_register(framework_module_t *mod);

int framework_discover_modules(void);
int framework_sort_modules(void);
framework_module_t *framework_get_module_by_index(unsigned int index);

/* Lifecycle callbacks */
int  call_init_all(void);
int  call_start_all(void);
void call_stop_all(void);
void call_deinit_all(void);

#endif /* FRAMEWORK_INTERNAL_H */
