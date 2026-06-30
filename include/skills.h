/*
 * skills.h — Skill loading and management
 *
 * Skills are markdown files in ~/.cgent/skills/.
 * Each skill has YAML frontmatter with name, description, and trigger.
 * The body text is the skill's instruction prompt.
 */
#ifndef SKILLS_H
#define SKILLS_H

#include <stdbool.h>
#include <stddef.h>

/* ── Skill definition ──────────────────────────────────────────── */

typedef struct {
    char *name;            /* Skill name */
    char *description;     /* Short description */
    char *trigger;         /* Trigger pattern (e.g. "/review") */
    char *instruction;     /* Skill prompt (body after frontmatter) */
    char *path;            /* File path */
} skill_t;

/* ── Skill list ────────────────────────────────────────────────── */

typedef struct {
    skill_t *skills;
    int count;
    int capacity;
} skill_list_t;

/* ── API ───────────────────────────────────────────────────────── */

/* Load all skills from a directory. Scans for .md files recursively. */
skill_list_t *skills_load_directory(const char *dir_path);

/* Find a skill by name (case-insensitive) */
skill_t *skills_find(skill_list_t *list, const char *name);

/* Find a skill by trigger pattern match */
skill_t *skills_find_by_trigger(skill_list_t *list, const char *input);

/* Build a combined system prompt from base prompt + triggered skills */
char *skills_build_prompt(skill_list_t *list, const char *base_prompt);

/* Free a skill list */
void skills_free(skill_list_t *list);

#endif /* SKILLS_H */
