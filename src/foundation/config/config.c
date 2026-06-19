/*
 * config.c — Simple YAML configuration parser
 *
 * No external YAML library. Reads config.yml into memory, parses
 * line-by-line. Handles sections, key:value, #comments, quotes.
 *
 * Two-pass logger init:
 *   1. framework_init() calls fw_log_init(NULL, stderr) before module init
 *   2. config_init() reads config.yml → fw_log_shutdown() + fw_log_init(file)
 *   3. Later modules log to the configured file
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "config.h"

#define CONFIG_PATH     "config.yml"
#define BUF_MAX         8192
#define LINE_MAX        512

static config_t g_config;

/* ── String Helpers ───────────────────────────────────────────────── */

static int is_space(int c) { return c == ' ' || c == '\t'; }

static char *trim(char *s)
{
    while (*s && is_space(*s)) s++;
    if (!*s) return s;
    char *end = s + os_strlen(s) - 1;
    while (end > s && is_space(*end)) *end-- = '\0';
    return s;
}

static void strip_comment(char *line)
{
    for (char *c = line; *c; c++) {
        if (*c == '#') { *c = '\0'; return; }
    }
}

static char *strip_quotes(char *s)
{
    size_t len = os_strlen(s);
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        s[len-1] = '\0';
        return s + 1;
    }
    /* Strip Unicode smart quotes: U+201C (") U+201D (") */
    if (len >= 6 && (unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x80
        && (unsigned char)s[2] == 0x9C
        && (unsigned char)s[len-3] == 0xE2 && (unsigned char)s[len-2] == 0x80
        && (unsigned char)s[len-1] == 0x9D) {
        s[len-3] = '\0';
        return s + 3;
    }
    return s;
}

/* Strip quotes from argv input: ASCII " and Unicode smart quotes */
static void strip_input_quotes(char *s)
{
    if (!s) return;
    size_t len = os_strlen(s);
    /* Leading " or U+201C */
    while (len >= 1) {
        if (s[0] == '"') {
            for (size_t i = 0; i < len; i++) s[i] = s[i+1];
            len--;
        }
        else if (len >= 3 && (unsigned char)s[0] == 0xE2
                 && (unsigned char)s[1] == 0x80
                 && (unsigned char)s[2] == 0x9C) {
            for (size_t i = 0; i < len - 2; i++) s[i] = s[i+3];
            len -= 3;
        } else break;
    }
    /* Trailing " or U+201D */
    while (len >= 1) {
        if (s[len-1] == '"') { s[len-1] = '\0'; len--; }
        else if (len >= 3 && (unsigned char)s[len-3] == 0xE2
                 && (unsigned char)s[len-2] == 0x80
                 && (unsigned char)s[len-1] == 0x9D) {
            s[len-3] = '\0'; len -= 3;
        } else break;
    }
}

static int parse_log_level(const char *s)
{
    if (!s) return FW_LOG_INFO;
    char c0 = (s[0] >= 'A' && s[0] <= 'Z') ? (char)(s[0] + 32) : s[0];
    switch (c0) {
    case 'd': return FW_LOG_DEBUG;
    case 'i': return FW_LOG_INFO;
    case 'w': return FW_LOG_WARN;
    case 'e': return FW_LOG_ERROR;
    case 'f': return FW_LOG_FATAL;
    default:  return FW_LOG_INFO;
    }
}

static int parse_int(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

/* ── Line Parser ──────────────────────────────────────────────────── */

/*
 * Walk through the buffer, calling the callback for each line.
 * Lines are separated by \n. The buffer is modified in-place
 * (comments stripped, NUL-terminated per line).
 */
typedef void (*line_cb_t)(char *line, void *user);

static void for_each_line(char *buf, line_cb_t cb, void *user)
{
    while (*buf) {
        /* Skip leading whitespace */
        while (*buf && (*buf == ' ' || *buf == '\t')) buf++;
        if (!*buf) break;

        char *start = buf;

        /* Find end of line */
        while (*buf && *buf != '\n') buf++;

        /* NUL-terminate */
        char saved = *buf;
        *buf = '\0';

        /* Strip inline comment */
        strip_comment(start);

        cb(start, user);

        if (saved == '\n') {
            *buf = saved;
            buf++;
        }
    }
}

/* ── YAML Parser State ────────────────────────────────────────────── */

typedef struct {
    char section[64];
    int  line_num;
} parser_state_t;

static void parse_line(char *line, void *user)
{
    parser_state_t *st = (parser_state_t *)user;
    st->line_num++;

    char *t = trim(line);
    if (!t || !*t) return;

    size_t len = os_strlen(t);

    /* Section header? (e.g. "logging:") */
    if (t[len - 1] == ':') {
        t[len - 1] = '\0';
        char *s = trim(t);
        size_t slen = os_strlen(s);
        if (slen < sizeof(st->section)) {
            os_memcpy(st->section, s, slen);
            st->section[slen] = '\0';
        }
        return;
    }

    /* Key: value pair */
    char *colon = NULL;
    for (char *c = t; *c; c++) {
        if (*c == ':') { colon = c; break; }
    }
    if (!colon) return;

    *colon = '\0';
    char *key = trim(t);
    char *val = strip_quotes(trim(colon + 1));

    if (st->section[0] == '\0') return;

    /* ── logging: ── */
    if (os_strcmp(st->section, "logging") == 0) {
        if (os_strcmp(key, "file") == 0) {
            size_t vlen = os_strlen(val);
            if (vlen < sizeof(g_config.log_file) && vlen > 0) {
                os_memcpy(g_config.log_file, val, vlen);
                g_config.log_file[vlen] = '\0';
            } else {
                g_config.log_file[0] = '\0';
            }
        } else if (os_strcmp(key, "level") == 0) {
            g_config.log_level = parse_log_level(val);
        }
    }
    /* ── threadpool: ── */
    else if (os_strcmp(st->section, "threadpool") == 0) {
        if (os_strcmp(key, "workers") == 0) {
            g_config.threadpool_workers = parse_int(val);
        }
    }
    /* ── llm: ── */
    else if (os_strcmp(st->section, "llm") == 0) {
        if (os_strcmp(key, "endpoint") == 0) {
            size_t vlen = os_strlen(val);
            if (vlen < sizeof(g_config.llm_endpoint)) {
                os_memcpy(g_config.llm_endpoint, val, vlen + 1);
            }
        } else if (os_strcmp(key, "api_key") == 0) {
            size_t vlen = os_strlen(val);
            if (vlen < sizeof(g_config.llm_api_key)) {
                os_memcpy(g_config.llm_api_key, val, vlen + 1);
            }
        } else if (os_strcmp(key, "model") == 0) {
            size_t vlen = os_strlen(val);
            if (vlen < sizeof(g_config.llm_model)) {
                os_memcpy(g_config.llm_model, val, vlen + 1);
            }
        }
    }
}

static int parse_config(const char *path)
{
    /* Defaults */
    g_config.log_file[0] = '\0';
    os_memcpy(g_config.log_file, "agent.log", 10);
    g_config.log_level   = FW_LOG_INFO;
    g_config.threadpool_workers = 0;
    g_config.llm_endpoint[0] = '\0';
    g_config.llm_api_key[0]  = '\0';
    g_config.llm_model[0]    = '\0';

    os_file_handle_t fh = os_file_open(path, "r");
    if (!fh) {
        LOG_WARN("Config: can't open '%s', using defaults", path);
        return -1;
    }

    char *buf = (char *)os_alloc(BUF_MAX);
    if (!buf) { os_file_close(fh); return -1; }

    size_t total = os_file_read(fh, buf, BUF_MAX - 1);
    os_file_close(fh);

    if (total == 0) {
        LOG_WARN("Config: '%s' is empty, using defaults", path);
        os_free(buf);
        return -1;
    }
    buf[total] = '\0';

    parser_state_t st = { .section = "", .line_num = 0 };
    for_each_line(buf, parse_line, &st);

    os_free(buf);

    LOG_INFO("Config: loaded '%s'", path);
    LOG_INFO("  log.file   = '%s'",
             g_config.log_file[0] ? g_config.log_file : "(stderr)");
    LOG_INFO("  log.level  = %d", g_config.log_level);
    LOG_INFO("  tp.workers = %d", g_config.threadpool_workers);

    return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

const config_t *config_get(void)
{
    return &g_config;
}

int config_set(const char *key, const char *value)
{
    if (!key || !value) return -1;

    /* Strip quotes from argv input (handles Windows smart quotes) */
    char clean_value[512];
    size_t vlen = os_strlen(value);
    if (vlen >= sizeof(clean_value)) vlen = sizeof(clean_value) - 1;
    os_memcpy(clean_value, value, vlen);
    clean_value[vlen] = '\0';
    strip_input_quotes(clean_value);
    value = clean_value;

    /* Parse dotted key */
    char section[64] = "";
    char subkey[64] = "";
    const char *dot = key;
    while (*dot && *dot != '.') dot++;
    size_t slen = (size_t)(dot - key);
    if (slen >= sizeof(section)) return -1;
    os_memcpy(section, key, slen);
    section[slen] = '\0';
    if (*dot == '.') {
        dot++;
        size_t klen = os_strlen(dot);
        if (klen >= sizeof(subkey)) return -1;
        os_memcpy(subkey, dot, klen + 1);
    }

    /* Parse existing config.yml to get current values */
    config_t cfg;
    os_memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = FW_LOG_INFO;

    /* Quick parse to populate cfg */
    os_file_handle_t fh = os_file_open(CONFIG_PATH, "r");
    if (fh) {
        char buf[BUF_MAX];
        size_t n = os_file_read(fh, buf, BUF_MAX - 1);
        os_file_close(fh);
        if (n > 0) {
            buf[n] = '\0';
            parser_state_t st = { .section = "", .line_num = 0 };
            /* Parse into a temporary g_config, then restore */
            config_t saved = g_config;
            os_memset(&g_config, 0, sizeof(g_config));
            g_config.log_level = FW_LOG_INFO;
            for_each_line(buf, parse_line, &st);
            cfg = g_config;
            g_config = saved;
        }
    }

    /* Apply the requested change */
    if (os_strcmp(section, "logging") == 0) {
        if (os_strcmp(subkey, "file") == 0) {
            size_t vlen = os_strlen(value);
            if (vlen < sizeof(cfg.log_file)) os_memcpy(cfg.log_file, value, vlen + 1);
        } else if (os_strcmp(subkey, "level") == 0) {
            cfg.log_level = parse_log_level(value);
        }
    } else if (os_strcmp(section, "threadpool") == 0) {
        if (os_strcmp(subkey, "workers") == 0) {
            cfg.threadpool_workers = parse_int(value);
        }
    } else if (os_strcmp(section, "llm") == 0) {
        if (os_strcmp(subkey, "endpoint") == 0) {
            size_t vlen = os_strlen(value);
            if (vlen < sizeof(cfg.llm_endpoint)) os_memcpy(cfg.llm_endpoint, value, vlen + 1);
        } else if (os_strcmp(subkey, "api_key") == 0) {
            size_t vlen = os_strlen(value);
            if (vlen < sizeof(cfg.llm_api_key)) os_memcpy(cfg.llm_api_key, value, vlen + 1);
        } else if (os_strcmp(subkey, "model") == 0) {
            size_t vlen = os_strlen(value);
            if (vlen < sizeof(cfg.llm_model)) os_memcpy(cfg.llm_model, value, vlen + 1);
        }
    }

    /* Write config.yml from template */
    os_file_handle_t out = os_file_open(CONFIG_PATH, "w");
    if (!out) return -1;

    char line[512];
    int n;

    n = os_snprintf(line, sizeof(line),
                    "# config.yml - Agent Framework configuration\n\n");
    os_file_write(out, line, (size_t)n);

    os_file_write(out, "logging:\n", 9);
    n = os_snprintf(line, sizeof(line), "  file: \"%s\"\n",
                    cfg.log_file[0] ? cfg.log_file : "agent.log");
    os_file_write(out, line, (size_t)n);

    const char *lvl_str = "info";
    if (cfg.log_level == FW_LOG_DEBUG) lvl_str = "debug";
    else if (cfg.log_level == FW_LOG_WARN)  lvl_str = "warn";
    else if (cfg.log_level == FW_LOG_ERROR) lvl_str = "error";
    else if (cfg.log_level == FW_LOG_FATAL) lvl_str = "fatal";
    n = os_snprintf(line, sizeof(line), "  level: %s\n", lvl_str);
    os_file_write(out, line, (size_t)n);

    os_file_write(out, "\nthreadpool:\n", 13);
    n = os_snprintf(line, sizeof(line), "  workers: %d\n", cfg.threadpool_workers);
    os_file_write(out, line, (size_t)n);

    os_file_write(out, "\nllm:\n", 6);
    n = os_snprintf(line, sizeof(line), "  endpoint: \"%s\"\n",
                    cfg.llm_endpoint[0] ? cfg.llm_endpoint
                                        : "http://localhost:8080/v1");
    os_file_write(out, line, (size_t)n);
    n = os_snprintf(line, sizeof(line), "  api_key: \"%s\"\n",
                    cfg.llm_api_key[0] ? cfg.llm_api_key : "");
    os_file_write(out, line, (size_t)n);
    n = os_snprintf(line, sizeof(line), "  model: \"%s\"\n",
                    cfg.llm_model[0] ? cfg.llm_model : "gpt-4");
    os_file_write(out, line, (size_t)n);

    os_file_close(out);
    return 0;
}

void config_show(void)
{
    os_printf("=== Config ===\n");
    os_printf("  log.file   = %s\n",
              g_config.log_file[0] ? g_config.log_file : "(stderr)");
    os_printf("  log.level  = %d\n", g_config.log_level);
    os_printf("  tp.workers = %d\n", g_config.threadpool_workers);
    os_printf("  llm.endpoint = %s\n",
              g_config.llm_endpoint[0] ? g_config.llm_endpoint : "(unset)");
    os_printf("  llm.api_key  = %s\n",
              g_config.llm_api_key[0] ? "***" : "(unset)");
    os_printf("  llm.model    = %s\n",
              g_config.llm_model[0] ? g_config.llm_model : "(unset)");
}

/* ── Module Registration ──────────────────────────────────────────── */

static void on_start_done(framework_event_type_t type,
                          const void *data, size_t data_size,
                          void *user_data);

static int config_init(framework_module_t *mod)
{
    (void)mod;

    parse_config(CONFIG_PATH);

    /* Two-pass: shutdown stderr logger, re-init with configured file */
    fw_log_shutdown();
    fw_log_init(
        g_config.log_file[0] ? g_config.log_file : NULL,
        g_config.log_level
    );

    /* Subscribe to framework lifecycle events */
    framework_event_subscribe(FW_EVENT_START_DONE, on_start_done, 0, NULL);

    return 0;
}

static void on_start_done(framework_event_type_t type,
                          const void *data, size_t data_size,
                          void *user_data)
{
    (void)type; (void)data_size; (void)user_data;
    const int *count = (const int *)data;
    LOG_INFO("Config: event START_DONE (%d module%s)",
             count ? *count : 0, (count && *count == 1) ? "" : "s");
}

static int config_start(framework_module_t *mod)
{
    (void)mod;

    /* Now all modules have inited — publish config for subscribers */
    framework_event_publish(FW_EVENT_CONFIG_LOADED, &g_config, sizeof(g_config));

    LOG_INFO("Config: ready");
    return 0;
}

/* Priority 25: inits before threadpool(20) and logger(10).
   The two-pass pattern (stderr→config→file) is encapsulated here. */
framework_module_t config_mod = {
    .name     = "config",
    .version  = 0x00010000,
    .priority = 25,
    .state    = FRAMEWORK_STATE_UNLOADED,
    .init     = config_init,
    .start    = config_start,
    .loop     = NULL,
    .stop     = NULL,
    .deinit   = NULL,
    .ctx      = &g_config,
    .id       = 0,
    .next     = NULL,
};

MODULE_REGISTER(config_mod);
