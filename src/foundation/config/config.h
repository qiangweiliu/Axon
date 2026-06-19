/*
 * config.h — Configuration module public API
 *
 * Foundation layer (priority=25). Parses config.yml at init time
 * and reconfigure logger via the two-pass pattern:
 *   stderr → read config → fw_log_shutdown + fw_log_init(file)
 */

#ifndef FOUNDATION_CONFIG_H
#define FOUNDATION_CONFIG_H

#include <stddef.h>

typedef struct {
    /* logging: */
    char     log_file[256];
    int      log_level;

    /* threadpool: */
    int      threadpool_workers;   /* 0 = auto-detect */

    /* llm: */
    char     llm_endpoint[256];
    char     llm_api_key[256];
    char     llm_model[128];
} config_t;

/* Returns the global parsed config. NULL before config module init. */
const config_t *config_get(void);

/* Write a config value back to config.yml. Key is dotted: "llm.endpoint".
   Returns 0 on success, -1 on failure. */
int config_set(const char *key, const char *value);

/* Print current config to stdout. */
void config_show(void);

#endif /* FOUNDATION_CONFIG_H */
