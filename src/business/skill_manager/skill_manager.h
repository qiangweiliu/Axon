/*
 * skill_manager.h — Skill registry: scan, index, and load skills
 *
 * Business layer (priority=360). Scans SKILL.md files from a configurable
 * directory, parses YAML frontmatter (name, description, category), indexes
 * them, and builds a system-prompt block so LLM knows what skills are
 * available.
 *
 * Skill file format (Hermes-compatible SKILL.md):
 *   ---
 *   name: skill-name
 *   description: One-line summary
 *   category: category-name
 *   ---
 *   <markdown body — instructions the model should follow>
 *
 * Directory layout:
 *   data/skills/
 *     category-name/
 *       skill-name/SKILL.md
 */

#ifndef BUSINESS_SKILL_MANAGER_H
#define BUSINESS_SKILL_MANAGER_H

#include <stddef.h>

/* Max skills the registry can hold */
#define SKILL_MAX 128

/* Per-skill metadata */
typedef struct {
    char name[64];
    char description[256];
    char category[64];
    char path[512];         /* absolute path to SKILL.md */
    char *body_cache;       /* cached body content (NULL = not cached), freed on scan/deinit */
    size_t body_cache_len;  /* length of cached body */
} skill_entry_t;

/*
 * Scan the skills directory and rebuild the index.
 * Returns the number of skills found, or -1 on error.
 * Called automatically during module init; call again to refresh.
 */
int skill_scan(void);

/*
 * Get the skills index string for the system prompt.
 * Returns a NUL-terminated string like:
 *   == Skills ==
 *   category-one:
 *     - skill-name: description
 *   category-two:
 *     - another-skill: does something
 *
 *   (Load any skill with [SKILL: name] in your response.)
 *
 * Returns NULL if no skills are loaded.
 * The returned pointer is valid until the next skill_scan() call.
 */
const char *skill_get_index(void);

/*
 * Load a skill's full content by name.
 * Returns a malloc'd NUL-terminated string (caller frees with skill_free_content),
 * or NULL if the skill is not found.
 */
char *skill_load(const char *name);

/*
 * Free content returned by skill_load().
 */
void skill_free_content(char *content);

/*
 * Get the number of loaded skills.
 */
int skill_count(void);

/*
 * Get skill entry by index (0..skill_count()-1).
 * Returns NULL if index is out of range.
 */
const skill_entry_t *skill_get(int index);

/*
 * Set the skills directory path.
 * Default: "data/skills" (relative to cwd).
 * Call before module init to override.
 */
void skill_set_dir(const char *path);

/*
 * Get a one-line summary of available skills (names only, no descriptions).
 * Format: "creative: novel-create, novel-engine, ... | devops: ..."
 * Returns a pointer to internal buffer (valid until next skill_scan()).
 */
const char *skill_get_names_line(void);

/*
 * Register list_skills and load_skill as tools in the tool_manager.
 * Called from skill_manager_start() after skill index is built.
 */
int skill_tools_register(void);

#endif /* BUSINESS_SKILL_MANAGER_H */
