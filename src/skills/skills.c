/*
 * skills.c — Skill loading from ~/.cgent/skills/
 *
 * Skill directory structure:
 *   ~/.cgent/skills/
 *     code-review/           ← directory name = skill name
 *       SKILL.md             ← skill definition (YAML frontmatter + body)
 *       scripts/             ← optional helper scripts
 *     simplify/
 *       SKILL.md
 *
 * The SKILL.md file uses the same format as AGENTS.md:
 * YAML frontmatter with name, description, plus body text for instructions.
 */
#include "skills.h"
#include "config.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Parsing ────────────────────────────────────────────────────── */

/* Parse a SKILL.md file. 'dir_name' is the skill directory name,
 * used as fallback if frontmatter doesn't specify a name. */
static skill_t *skill_parse_file(const char *filepath, const char *dir_name) {
    agent_md_t *am = agent_md_parse(filepath);
    if (!am) return NULL;

    skill_t *skill = calloc(1, sizeof(skill_t));

    /* Name: frontmatter > directory name */
    if (am->name && am->name[0]) {
        skill->name = am->name;
        am->name = NULL;
    } else {
        skill->name = strdup(dir_name);
    }

    /* Description */
    if (am->description) {
        skill->description = am->description;
        am->description = NULL;
    }

    /* Instruction (body text) */
    if (am->instruction) {
        skill->instruction = am->instruction;
        am->instruction = NULL;
    }

    skill->path = strdup(filepath);

    /* Trigger from name: "/" + name */
    size_t tlen = strlen(skill->name) + 2;
    skill->trigger = malloc(tlen);
    snprintf(skill->trigger, tlen, "/%s", skill->name);

    agent_md_free(am);
    return skill;
}

/* ── Directory scanning ────────────────────────────────────────── */

static void scan_skills_root(skill_list_t *list, const char *skills_dir) {
    DIR *dir = opendir(skills_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;  /* Skip hidden */

        char *skill_dir = os_path_join(skills_dir, entry->d_name);
        if (!skill_dir) continue;

        struct stat st;
        if (stat(skill_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(skill_dir);
            continue;  /* Only directories are skills */
        }

        /* Look for SKILL.md inside the skill directory */
        char *skill_file = os_path_join(skill_dir, "SKILL.md");
        if (skill_file && os_path_exists(skill_file)) {
            skill_t *skill = skill_parse_file(skill_file, entry->d_name);
            if (skill) {
                if (list->count >= list->capacity) {
                    list->capacity = list->capacity ? list->capacity * 2 : 16;
                    list->skills = realloc(list->skills,
                        list->capacity * sizeof(skill_t));
                }
                list->skills[list->count++] = *skill;
                free(skill);
            }
        }
        free(skill_file);
        free(skill_dir);
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

    scan_skills_root(list, dir_path);
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
