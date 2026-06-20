/* handlers.c — Command handler implementations */

#include "agent_framework.h"
#include "tool_manager.h"
#include "memory.h"
#include "agent_private.h"
#include "os_api.h"
#include <string.h>

void handle_echo(const char *msg, char *out, size_t out_len)
{
    char args[512];
    os_snprintf(args, sizeof(args), "{\"msg\":\"%s\"}", msg ? msg : "");
    char result[TOOL_RESULT_MAX];
    if (tool_call("echo", args, result, sizeof(result)) >= 0) {
        if (out) os_snprintf(out, out_len, YLW "%s" RST, result);
    }
}

void handle_remember(const char *content, char *out, size_t out_len)
{
    if (!content || !*content) return;
    memory_entry_t e;
    os_memset(&e, 0, sizeof(e));
    os_memcpy(e.type, "note", 5);
    size_t clen = os_strlen(content);
    if (clen >= MEMORY_CONTENT_MAX) clen = MEMORY_CONTENT_MAX - 1;
    os_memcpy(e.content, content, clen);
    char id[MEMORY_ID_MAX];
    if (memory_store(&e, id, sizeof(id)) == 0) {
        if (out) os_snprintf(out, out_len, GRY "✓ stored" RST);
    }
}

int handle_recall(const char *query, char *out, size_t out_len)
{
    memory_entry_t results[4];
    int found;
    if (memory_search(query, results, 4, &found) == 0 && found > 0) {
        if (out) {
            size_t pos = 0;
            pos += os_snprintf(out + pos, out_len - pos,
                               GRY "%d result(s)" RST, found);
            for (int i = 0; i < found && pos < out_len; i++) {
                pos += os_snprintf(out + pos, out_len - pos,
                                   "\n  " GRY "·" RST " %s",
                                   results[i].content);
            }
        }
        return found;
    }
    return 0;
}

/* ── Hermes-style bounded memory commands ─────────────────────────── */

void handle_note(const char *text, char *out, size_t out_len)
{
    if (!text || !*text) return;

    /* Skip if identical text already exists (dedup) */
    for (int i = 0; i < g_ctx->mem.count; i++) {
        if (os_strcmp(g_ctx->mem.entries[i], text) == 0) {
            if (out) os_snprintf(out, out_len, GRY "✓ already noted" RST);
            return;
        }
    }

    if (memfile_add(&g_ctx->mem, text) != 0) {
        if (out) os_snprintf(out, out_len,
            RED "%% memory full" RST " — use " GRY "forget" RST " to free space");
        return;
    }
    memfile_save(&g_ctx->mem);
    char usage[64];
    memfile_usage(&g_ctx->mem, usage, sizeof(usage));
    if (out) os_snprintf(out, out_len, GRY "✓ noted  (%s)" RST, usage);
}

void handle_profile(const char *text, char *out, size_t out_len)
{
    if (!text || !*text) return;

    /* Extract key (text before '=') for replace lookup */
    const char *eq = os_strchr(text, '=');
    if (eq) {
        /* Try replace first (updates existing key in place) */
        char key[256];
        size_t klen = (size_t)(eq - text);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        os_memcpy(key, text, klen);
        key[klen] = '\0';

        if (memfile_replace(&g_ctx->user, key, text) == 0) {
            memfile_save(&g_ctx->user);
            char usage[64];
            memfile_usage(&g_ctx->user, usage, sizeof(usage));
            if (out) os_snprintf(out, out_len, GRY "✓ profile updated  (%s)" RST, usage);
            return;
        }
    }

    /* Key not found — append as new entry */
    if (memfile_add(&g_ctx->user, text) != 0) {
        if (out) os_snprintf(out, out_len,
            RED "%% profile full" RST " — use " GRY "forget" RST " to free space");
        return;
    }
    memfile_save(&g_ctx->user);
    char usage[64];
    memfile_usage(&g_ctx->user, usage, sizeof(usage));
    if (out) os_snprintf(out, out_len, GRY "✓ profile saved  (%s)" RST, usage);
}

/*
 * replace <key> <text>
 * Find the memory entry containing <key> and replace its content.
 * Targets memory (not profile). Use "forget -u" + "profile" for user edits.
 */
void handle_replace(const char *args, char *out, size_t out_len)
{
    if (!args || !*args) return;

    /* Split on first space: key = before, text = after */
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (*p != ' ') {
        if (out) os_snprintf(out, out_len,
            GRY "usage: replace <key> <new text>" RST);
        return;
    }

    size_t key_len = (size_t)(p - args);
    const char *text = p;
    while (*text == ' ') text++;
    if (!*text) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: replace <key> <new text>" RST);
        return;
    }

    char key_buf[256];
    size_t kc = key_len < sizeof(key_buf) - 1
                ? key_len : sizeof(key_buf) - 1;
    os_memcpy(key_buf, args, kc);
    key_buf[kc] = '\0';

    if (memfile_replace(&g_ctx->mem, key_buf, text) == 0) {
        memfile_save(&g_ctx->mem);
        char usage[64];
        memfile_usage(&g_ctx->mem, usage, sizeof(usage));
        if (out) os_snprintf(out, out_len,
            GRY "✓ replaced  (%s)" RST, usage);
    } else {
        if (out) os_snprintf(out, out_len,
            RED "%% replace failed" RST " — key not found or would exceed limit");
    }
}

/*
 * forget [-m|-u] <substring>
 *   -m  : remove from memory only (default if omitted, i.e. both)
 *   -u  : remove from user profile only
 * Default (no flag): remove from both
 */
void handle_forget(const char *raw, char *out, size_t out_len)
{
    if (!raw || !*raw) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: forget [-m|-u] <substring>" RST);
        return;
    }

    int do_mem  = 0;
    int do_user = 0;
    const char *sub = raw;

    /* Parse optional flag */
    if (*raw == '-') {
        if (raw[1] == 'm' && (raw[2] == ' ' || raw[2] == '\0')) {
            do_mem = 1;
            sub = raw[2] ? raw + 3 : "";
        } else if (raw[1] == 'u' && (raw[2] == ' ' || raw[2] == '\0')) {
            do_user = 1;
            sub = raw[2] ? raw + 3 : "";
        } else {
            if (out) os_snprintf(out, out_len,
                GRY "usage: forget [-m|-u] <substring>" RST);
            return;
        }
    } else {
        do_mem = 1;
        do_user = 1;
    }

    while (*sub == ' ') sub++;
    if (!*sub) {
        if (out) os_snprintf(out, out_len,
            GRY "usage: forget [-m|-u] <substring>" RST);
        return;
    }

    int total = 0;
    if (do_mem) {
        int rm = memfile_remove(&g_ctx->mem, sub);
        if (rm > 0) memfile_save(&g_ctx->mem);
        total += rm;
    }
    if (do_user) {
        int ru = memfile_remove(&g_ctx->user, sub);
        if (ru > 0) memfile_save(&g_ctx->user);
        total += ru;
    }

    if (total > 0) {
        if (out) os_snprintf(out, out_len,
            GRY "✓ removed %d entry" RST, total);
    } else {
        if (out) os_snprintf(out, out_len,
            GRY "nothing matched '%s'" RST, sub);
    }
}

void handle_notes(char *out, size_t out_len)
{
    char mu[64], uu[64];
    memfile_usage(&g_ctx->mem, mu, sizeof(mu));
    memfile_usage(&g_ctx->user, uu, sizeof(uu));

    size_t pos = 0;
    pos += os_snprintf(out + pos, out_len - pos,
        BLD "Memory" RST "  (" GRY "%s" RST ")" "\n", mu);
    for (int i = 0; i < g_ctx->mem.count && pos < out_len; i++) {
        pos += os_snprintf(out + pos, out_len - pos,
            "  " DIM "%d." RST " %s\n", i + 1, g_ctx->mem.entries[i]);
    }
    pos += os_snprintf(out + pos, out_len - pos,
        BLD "Profile" RST " (" GRY "%s" RST ")" "\n", uu);
    for (int i = 0; i < g_ctx->user.count && pos < out_len; i++) {
        pos += os_snprintf(out + pos, out_len - pos,
            "  " DIM "%d." RST " %s\n", i + 1, g_ctx->user.entries[i]);
    }
}
