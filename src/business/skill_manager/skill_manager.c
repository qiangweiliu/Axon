/*
 * skill_manager.c — Skill registry: scan, index, and load skills
 *
 * Scans SKILL.md files from a configurable directory, parses
 * YAML frontmatter (name, description, category), indexes them,
 * and builds a system-prompt block so the LLM knows what skills
 * are available.
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "skill_manager.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#define SKILL_INDEX_BUF 8192
#define SKILL_CONTENT_BUF 16384
#define LINE_MAX 512

typedef struct {
    skill_entry_t  entries[SKILL_MAX];
    int            count;
    char           index_buf[SKILL_INDEX_BUF];
    int            index_dirty;
    char           skills_dir[512];
    int            inited;
} skill_manager_ctx_t;

static skill_manager_ctx_t *g_ctx = NULL;

/* ── Default skills directory ─────────────────────────────────────── */

void skill_set_dir(const char *path)
{
    if (!g_ctx || !path) return;
    size_t len = os_strlen(path);
    if (len >= sizeof(g_ctx->skills_dir)) len = sizeof(g_ctx->skills_dir) - 1;
    os_memcpy(g_ctx->skills_dir, path, len);
    g_ctx->skills_dir[len] = '\0';
    g_ctx->index_dirty = 1;
}

/* ── Frontmatter parser ───────────────────────────────────────────── */

/* Trim leading/trailing whitespace in-place */
static char *trim(char *s)
{
    while (*s && (*s == ' ' || *s == '\t')) s++;
    if (!*s) return s;
    char *end = s + os_strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r')) end--;
    *(end + 1) = '\0';
    return s;
}

/* Parse YAML frontmatter delimited by `---` from raw file content.
 * Returns pointer past closing `---`, or content if no frontmatter.
 * Fills name/desc/category from frontmatter keys. */
static const char *parse_frontmatter(const char *content,
                                     char *name, size_t name_len,
                                     char *desc, size_t desc_len,
                                     char *cat, size_t cat_len)
{
    if (!content || os_strncmp(content, "---", 3) != 0)
        return content; /* no frontmatter */

    const char *p = content + 3;
    if (*p == '\n') p++;

    /* Find closing --- */
    const char *end = strstr(p, "\n---");
    if (!end) {
        /* Try ending at file boundary */
        end = strstr(p, "---");
        if (!end || end == p) return content;
    }

    /* Parse key:value lines between the --- markers */
    const char *line = p;
    while (line < end) {
        /* Find end of this line */
        const char *nl = strchr(line, '\n');
        if (!nl) nl = end;

        /* Extract the line text */
        char buf[256];
        size_t llen = (size_t)(nl - line);
        if (llen >= sizeof(buf)) llen = sizeof(buf) - 1;
        os_memcpy(buf, line, llen);
        buf[llen] = '\0';
        char *t = trim(buf);
        if (*t && *t != '#') {
            char *colon = strchr(t, ':');
            if (colon) {
                *colon = '\0';
                char *key = trim(t);
                char *val = trim(colon + 1);
                if (os_strcmp(key, "name") == 0) {
                    size_t vlen = os_strlen(val);
                    if (vlen >= name_len) vlen = name_len - 1;
                    os_memcpy(name, val, vlen);
                    name[vlen] = '\0';
                } else if (os_strcmp(key, "description") == 0) {
                    size_t vlen = os_strlen(val);
                    if (vlen >= desc_len) vlen = desc_len - 1;
                    os_memcpy(desc, val, vlen);
                    desc[vlen] = '\0';
                } else if (os_strcmp(key, "category") == 0) {
                    size_t vlen = os_strlen(val);
                    if (vlen >= cat_len) vlen = cat_len - 1;
                    os_memcpy(cat, val, vlen);
                    cat[vlen] = '\0';
                }
            }
        }

        line = nl + 1;
    }

    /* Return content after closing --- */
    const char *after = end + 4; /* skip "\n---" */
    if (*after == '\n') after++;
    return after;
}

/* ── Directory scan ───────────────────────────────────────────────── */

/* Recursively find all SKILL.md files under a directory.
 * Uses os_dir_open/close/next from the platform layer. */
static int scan_skills_in_dir(const char *dir_path, int depth)
{
    if (depth > 8) return 0; /* safety limit */
    int found = 0;

    os_dir_handle_t dh = os_dir_open(dir_path);
    if (!dh) return 0;

    const char *entry;
    while ((entry = os_dir_next(dh)) != NULL) {
        /* Skip . and .. */
        if (os_strcmp(entry, ".") == 0 || os_strcmp(entry, "..") == 0)
            continue;

        /* Build full path */
        char full[512];
        int n = os_snprintf(full, sizeof(full), "%s/%s", dir_path, entry);
        if (n < 0 || (size_t)n >= sizeof(full)) continue;

        /* Check if it's a directory (try opening as dir first) */
        os_dir_handle_t dh = os_dir_open(full);
        if (dh) {
            os_dir_close(dh);
            /* It's a directory — recurse */
            scan_skills_in_dir(full, depth + 1);
            continue;
        }

        /* It's a file — check if it's SKILL.md */
        if (os_strcmp(entry, "SKILL.md") != 0 &&
            os_strcmp(entry, "skill.md") != 0)
            continue;

        /* Read the file */
        os_file_handle_t fh = os_file_open(full, "r");
        if (!fh) continue;

        char buf[SKILL_CONTENT_BUF];
        size_t nread = os_file_read(fh, buf, sizeof(buf) - 1);
        os_file_close(fh);
        if (nread == 0) continue;
        buf[nread] = '\0';

        /* Parse frontmatter */
        char name[64] = "", desc[256] = "", cat[64] = "";
        parse_frontmatter(buf, name, sizeof(name),
                          desc, sizeof(desc),
                          cat, sizeof(cat));

        if (!name[0]) continue; /* must have a name */

        /* Use directory name as fallback category */
        if (!cat[0]) {
            /* Extract parent dir name */
            const char *last_slash = strrchr(dir_path, '/');
            if (last_slash) {
                const char *prev_slash = last_slash;
                while (prev_slash > dir_path && *(prev_slash - 1) != '/')
                    prev_slash--;
                size_t plen = (size_t)(last_slash - prev_slash);
                if (plen >= sizeof(cat)) plen = sizeof(cat) - 1;
                os_memcpy(cat, prev_slash, plen);
                cat[plen] = '\0';
            }
        }

        /* Store entry */
        if (g_ctx->count < SKILL_MAX) {
            skill_entry_t *e = &g_ctx->entries[g_ctx->count];
            size_t nlen = os_strlen(name);
            if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
            os_memcpy(e->name, name, nlen);
            e->name[nlen] = '\0';

            size_t dlen = os_strlen(desc);
            if (dlen >= sizeof(e->description)) dlen = sizeof(e->description) - 1;
            os_memcpy(e->description, desc, dlen);
            e->description[dlen] = '\0';

            size_t clen = os_strlen(cat);
            if (clen >= sizeof(e->category)) clen = sizeof(e->category) - 1;
            os_memcpy(e->category, cat, clen);
            e->category[clen] = '\0';

            size_t plen = os_strlen(full);
            if (plen >= sizeof(e->path)) plen = sizeof(e->path) - 1;
            os_memcpy(e->path, full, plen);
            e->path[plen] = '\0';

            g_ctx->count++;
            found++;
        }
    }

    os_dir_close(dh);
    return found;
}

/* ── Public API ────────────────────────────────────────────────────── */

int skill_scan(void)
{
    if (!g_ctx) return -1;

    g_ctx->count = 0;
    g_ctx->index_dirty = 1;

    const char *dir = g_ctx->skills_dir;
    if (!dir || !dir[0])
        dir = "data/skills";

    /* Check if directory exists */
    os_dir_handle_t dh = os_dir_open(dir);
    if (!dh) {
        LOG_INFO("Skills: directory '%s' not found, no skills loaded", dir);
        return 0;
    }
    os_dir_close(dh);

    int found = scan_skills_in_dir(dir, 0);
    LOG_INFO("Skills: scanned '%s' — %d skill(s) found", dir, found);
    return found;
}

const char *skill_get_index(void)
{
    if (!g_ctx) return NULL;
    if (!g_ctx->index_dirty) {
        if (g_ctx->index_buf[0]) return g_ctx->index_buf;
        return NULL;
    }

    if (g_ctx->count == 0) {
        g_ctx->index_buf[0] = '\0';
        g_ctx->index_dirty = 0;
        return NULL;
    }

    /* Build index: grouped by category */
    int pos = 0;
    pos += os_snprintf(g_ctx->index_buf + pos,
                       sizeof(g_ctx->index_buf) - pos,
                       "## Available Skills\n"
                       "Below is a categorized index of reusable skills. "
                       "When a skill matches or is even partially relevant to "
                       "your task, load it with [SKILL: <name>] in your "
                       "response and follow its instructions.\n"
                       "\n");

    /* Track which categories we've output */
    char seen_cats[SKILL_MAX][64];
    int ncats = 0;

    for (int i = 0; i < g_ctx->count; i++) {
        skill_entry_t *e = &g_ctx->entries[i];

        /* Check if we've already printed this category */
        int found = 0;
        for (int c = 0; c < ncats; c++) {
            if (os_strcmp(seen_cats[c], e->category) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (ncats < SKILL_MAX) {
                size_t clen = os_strlen(e->category);
                if (clen >= sizeof(seen_cats[ncats]))
                    clen = sizeof(seen_cats[ncats]) - 1;
                os_memcpy(seen_cats[ncats], e->category, clen);
                seen_cats[ncats][clen] = '\0';
                ncats++;

                pos += os_snprintf(g_ctx->index_buf + pos,
                                   sizeof(g_ctx->index_buf) - pos,
                                   "  %s:\n", e->category);
            }
        }

        pos += os_snprintf(g_ctx->index_buf + pos,
                           sizeof(g_ctx->index_buf) - pos,
                           "    - %s: %s\n",
                           e->name, e->description);
        if (pos >= (int)sizeof(g_ctx->index_buf) - 2) break;
    }

    pos += os_snprintf(g_ctx->index_buf + pos,
                       sizeof(g_ctx->index_buf) - pos,
                       "\nTo load a skill, include [SKILL: <name>] in your "
                       "response. The skill's full content will be injected "
                       "into the conversation.");
    g_ctx->index_dirty = 0;
    return g_ctx->index_buf;
}

char *skill_load(const char *name)
{
    if (!g_ctx || !name) return NULL;

    /* Find the skill */
    skill_entry_t *match = NULL;
    for (int i = 0; i < g_ctx->count; i++) {
        if (os_strcmp(g_ctx->entries[i].name, name) == 0) {
            match = &g_ctx->entries[i];
            break;
        }
    }
    if (!match) return NULL;

    /* Read the file */
    os_file_handle_t fh = os_file_open(match->path, "r");
    if (!fh) return NULL;

    char buf[SKILL_CONTENT_BUF];
    size_t nread = os_file_read(fh, buf, sizeof(buf) - 1);
    os_file_close(fh);
    if (nread == 0) return NULL;
    buf[nread] = '\0';

    /* Strip frontmatter, keep body */
    char name_discard[64], desc_discard[256], cat_discard[64];
    const char *body = parse_frontmatter(buf,
                                          name_discard, sizeof(name_discard),
                                          desc_discard, sizeof(desc_discard),
                                          cat_discard, sizeof(cat_discard));

    /* Allocate and return the body */
    size_t blen = os_strlen(body);
    char *result = (char *)os_alloc(blen + 1);
    if (!result) return NULL;
    os_memcpy(result, body, blen);
    result[blen] = '\0';
    return result;
}

void skill_free_content(char *content)
{
    if (content) os_free(content);
}

int skill_count(void)
{
    return g_ctx ? g_ctx->count : 0;
}

const skill_entry_t *skill_get(int index)
{
    if (!g_ctx || index < 0 || index >= g_ctx->count) return NULL;
    return &g_ctx->entries[index];
}

/* ── Module Registration ──────────────────────────────────────────── */

static int skill_manager_init(framework_module_t *mod)
{
    g_ctx = (skill_manager_ctx_t *)os_calloc(1, sizeof(skill_manager_ctx_t));
    if (!g_ctx) return -1;
    mod->ctx = g_ctx;

    /* Default skills directory */
    os_memcpy(g_ctx->skills_dir, "data/skills", 12);

    /* Override from config if available */
    const config_t *cfg = config_get();
    if (cfg && cfg->skills_dir[0]) {
        size_t len = os_strlen(cfg->skills_dir);
        if (len >= sizeof(g_ctx->skills_dir))
            len = sizeof(g_ctx->skills_dir) - 1;
        os_memcpy(g_ctx->skills_dir, cfg->skills_dir, len);
        g_ctx->skills_dir[len] = '\0';
    }

    skill_scan();
    g_ctx->inited = 1;
    return 0;
}

static int skill_manager_start(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("SkillManager: %d skill(s) indexed", g_ctx ? g_ctx->count : 0);
    return 0;
}

static int skill_manager_deinit(framework_module_t *mod)
{
    (void)mod;
    if (g_ctx) {
        os_free(g_ctx);
        g_ctx = NULL;
    }
    return 0;
}

framework_module_t skill_manager_mod = {
    .name = "skill_manager",
    .version = 0x00010000,
    .state = FRAMEWORK_STATE_UNLOADED,
    .init = skill_manager_init,
    .start = skill_manager_start,
    .loop = NULL,
    .stop = NULL,
    .deinit = skill_manager_deinit,
    .ctx = NULL,
    .id = 0,
    .next = NULL,
};
MODULE_REGISTER(skill_manager_mod);

const char *skill_get_names_line(void)
{
    if (!g_ctx || g_ctx->count == 0) return NULL;

    /* Use a static buffer in the context */
    static char line_buf[512];
    int pos = 0;
    char seen_cats[SKILL_MAX][64];
    int ncats = 0;

    for (int i = 0; i < g_ctx->count && pos < (int)sizeof(line_buf) - 4; i++) {
        skill_entry_t *e = &g_ctx->entries[i];
        int found = 0;
        for (int c = 0; c < ncats; c++) {
            if (os_strcmp(seen_cats[c], e->category) == 0) { found = 1; break; }
        }
        if (!found && ncats < SKILL_MAX) {
            if (pos > 0) pos += os_snprintf(line_buf + pos, sizeof(line_buf) - pos, " | ");
            size_t cl = os_strlen(e->category);
            if (cl >= sizeof(seen_cats[ncats])) cl = sizeof(seen_cats[ncats]) - 1;
            os_memcpy(seen_cats[ncats], e->category, cl);
            seen_cats[ncats][cl] = '\0';
            ncats++;
            pos += os_snprintf(line_buf + pos, sizeof(line_buf) - pos, "%s:", e->category);
        }
        pos += os_snprintf(line_buf + pos, sizeof(line_buf) - pos, " %s", e->name);
    }
    line_buf[pos < (int)sizeof(line_buf) ? (size_t)pos : sizeof(line_buf) - 1] = '\0';
    return line_buf;
}
