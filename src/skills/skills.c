/*
 * skills.c — Skill loading from ~/.cgent/skills/
 *
 * Scans the skills directory for .md files, parses YAML frontmatter,
 * and builds a skill list available to the agent.
 */
#include "skills.h"
#include "config.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── YAML frontmatter parsing for skills ──────────────────────── */

/* Parse a skill-specific YAML frontmatter.
 * Reuses the agent_md_t parser but also extracts the 'trigger' field.
 * The agent_md_parse function handles frontmatter + body split. */
static skill_t *skill_parse_file(const char *filepath) {
    agent_md_t *am = agent_md_parse(filepath);
    if (!am) return NULL;

    /* A valid skill must have at least a name */
    if (!am->name) {
        agent_md_free(am);
        return NULL;
    }

    skill_t *skill = calloc(1, sizeof(skill_t));
    skill->name = am->name;          /* Transfer ownership */
    am->name = NULL;
    skill->description = am->description;
    am->description = NULL;
    skill->instruction = am->instruction;
    am->instruction = NULL;
    skill->path = strdup(filepath);

    /* 'trigger' is not a standard agent_md field, so we need to
     * re-parse the frontmatter to extract it.
     * For now, derive trigger from name: "/" + name */
    size_t tlen = strlen(skill->name) + 2;
    skill->trigger = malloc(tlen);
    snprintf(skill->trigger, tlen, "/%s", skill->name);

    agent_md_free(am);
    return skill;
}

/* ── Directory scanning ────────────────────────────────────────── */

static void scan_directory(skill_list_t *list, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip hidden files/dirs */
        if (entry->d_name[0] == '.') continue;

        char *full_path = os_path_join(dir_path, entry->d_name);
        if (!full_path) continue;

        struct stat st;
        if (stat(full_path, &st) != 0) { free(full_path); continue; }

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectories */
            scan_directory(list, full_path);
        } else if (S_ISREG(st.st_mode)) {
            /* Check if it's a .md file */
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".md") == 0) {
                skill_t *skill = skill_parse_file(full_path);
                if (skill) {
                    /* Ensure capacity */
                    if (list->count >= list->capacity) {
                        list->capacity = list->capacity ? list->capacity * 2 : 16;
                        list->skills = realloc(list->skills,
                            list->capacity * sizeof(skill_t));
                    }
                    list->skills[list->count++] = *skill;
                    free(skill); /* Free wrapper, contents transferred */
                }
            }
        }
        free(full_path);
    }
    closedir(dir);
}

/* ── Public API ────────────────────────────────────────────────── */

skill_list_t *skills_load_directory(const char *dir_path) {
    skill_list_t *list = calloc(1, sizeof(skill_list_t));
    if (!list) return NULL;

    /* Create directory if it doesn't exist */
    if (!os_path_exists(dir_path)) {
        os_mkdir_p(dir_path);
        return list; /* Empty list — no skills yet */
    }

    scan_directory(list, dir_path);
    return list;
}

skill_t *skills_find(skill_list_t *list, const char *name) {
    if (!list || !name) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcasecmp(list->skills[i].name, name) == 0)
            return &list->skills[i];
    }
    return NULL;
}

skill_t *skills_find_by_trigger(skill_list_t *list, const char *input) {
    if (!list || !input) return NULL;
    for (int i = 0; i < list->count; i++) {
        const char *trigger = list->skills[i].trigger;
        if (trigger && trigger[0] && strncmp(input, trigger, strlen(trigger)) == 0)
            return &list->skills[i];
    }
    return NULL;
}

char *skills_build_prompt(skill_list_t *list, const char *base_prompt) {
    if (!base_prompt) return NULL;

    /* Estimate size: base prompt + all skill instructions */
    size_t total = strlen(base_prompt) + 256;
    for (int i = 0; i < list->count; i++) {
        if (list->skills[i].instruction)
            total += strlen(list->skills[i].instruction) + 64;
    }

    char *result = malloc(total);
    if (!result) return strdup(base_prompt);

    size_t pos = 0;

    /* List available skills at the top */
    if (list->count > 0) {
        pos += snprintf(result + pos, total - pos,
            "You have the following skills available:\n");
        for (int i = 0; i < list->count; i++) {
            pos += snprintf(result + pos, total - pos,
                "  - %s: %s (trigger: %s)\n",
                list->skills[i].name,
                list->skills[i].description ? list->skills[i].description : "",
                list->skills[i].trigger ? list->skills[i].trigger : "");
        }
        pos += snprintf(result + pos, total - pos,
            "To invoke a skill, say its trigger or name.\n\n");
    }

    /* Append base prompt */
    pos += snprintf(result + pos, total - pos, "%s", base_prompt);

    return result;
}

void skills_free(skill_list_t *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->skills[i].name);
        free(list->skills[i].description);
        free(list->skills[i].trigger);
        free(list->skills[i].instruction);
        free(list->skills[i].path);
    }
    free(list->skills);
    free(list);
}
