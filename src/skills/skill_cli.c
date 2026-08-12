/*
 * skill_cli.c — `cgent skill` subcommand: manage skills
 *
 * Usage:
 *   cgent skill list
 *   cgent skill add <name> [--description "desc"] [--instruction "body"] [--force]
 *   cgent skill remove <name>
 *   cgent skill show <name>
 *   cgent skill help
 *
 * Skills are stored as ~/.cgent/skills/<name>/SKILL.md with YAML
 * frontmatter (name, description) and an instruction body.
 */
#include "skills.h"
#include "config.h"
#include "platform.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Help ───────────────────────────────────────────────────────── */

static void skill_usage(void) {
    printf("Usage: cgent skill <command> [options]\n\n");
    printf("Manage cgent skills.\n\n");
    printf("Commands:\n");
    printf("  list                  List installed skills\n");
    printf("  add <name> ...        Create a new skill\n");
    printf("  remove <name>         Delete a skill\n");
    printf("  show <name>           Show a skill's full definition\n");
    printf("  help                  Show this help\n");
    printf("\nadd options:\n");
    printf("  --description <text>  Short description of the skill\n");
    printf("  --instruction <text>  Skill instructions (the prompt body)\n");
    printf("  -f, --force           Overwrite the skill if it already exists\n");
    printf("\nSkills directory: ~/.cgent/skills/<name>/SKILL.md\n");
}

/* ── Helpers ────────────────────────────────────────────────────── */

static char *skills_dir_path(void) {
    char *base = config_cgent_dir();
    if (!base) return NULL;
    char *dir = os_path_join(base, "skills");
    free(base);
    return dir;
}

/* Restrict skill names to safe, filesystem-friendly characters. */
static bool valid_skill_name(const char *name) {
    if (!name || !name[0] || name[0] == '.' || strstr(name, ".."))
        return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static int remove_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char *child = os_path_join(path, e->d_name);
        if (!child) { rc = -1; continue; }
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (remove_recursive(child) != 0) rc = -1;
            } else if (unlink(child) != 0) {
                rc = -1;
            }
        }
        free(child);
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

/* ── list ───────────────────────────────────────────────────────── */

static int skill_cli_list(void) {
    char *dir = skills_dir_path();
    if (!dir) {
        fprintf(stderr, "Error: cannot resolve skills directory\n");
        return 1;
    }
    skill_list_t *list = skills_load_directory(dir);
    if (!list) {
        fprintf(stderr, "Error: failed to load skills\n");
        free(dir);
        return 1;
    }
    if (list->count == 0) {
        printf("No skills installed.\n");
        printf("Add one with: cgent skill add <name> --description \"...\"\n");
        skills_free(list);
        free(dir);
        return 0;
    }

    printf("%-24s %-50s %-14s\n", "NAME", "DESCRIPTION", "TRIGGER");
    for (int i = 0; i < list->count; i++) {
        skill_t *s = &list->skills[i];
        printf("%-24s %-50s %-14s\n",
               s->name,
               s->description && s->description[0] ? s->description : "(no description)",
               s->trigger ? s->trigger : "");
    }
    printf("\n%d skill(s) installed in %s\n", list->count, dir);
    skills_free(list);
    free(dir);
    return 0;
}

/* ── add ────────────────────────────────────────────────────────── */

static int skill_cli_add(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent skill add <name> [--description \"...\"] "
                        "[--instruction \"...\"] [--force]\n");
        return 1;
    }
    const char *name = argv[0];
    const char *description = NULL;
    const char *instruction = NULL;
    bool force = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--description") == 0 && i + 1 < argc) {
            description = argv[++i];
        } else if (strcmp(argv[i], "--instruction") == 0 && i + 1 < argc) {
            instruction = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            force = true;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!valid_skill_name(name)) {
        fprintf(stderr, "Error: invalid skill name '%s' "
                        "(use letters, digits, '-', '_', '.'; "
                        "no leading '.', no '..')\n", name);
        return 1;
    }

    char *dir = skills_dir_path();
    if (!dir) {
        fprintf(stderr, "Error: cannot resolve skills directory\n");
        return 1;
    }
    char *skill_dir = os_path_join(dir, name);
    char *skill_file = skill_dir ? os_path_join(skill_dir, "SKILL.md") : NULL;
    if (!skill_dir || !skill_file) {
        fprintf(stderr, "Error: out of memory\n");
        free(skill_file);
        free(skill_dir);
        free(dir);
        return 1;
    }

    if (os_path_exists(skill_file)) {
        if (!force) {
            fprintf(stderr, "Error: skill '%s' already exists (use --force to overwrite)\n",
                    name);
            free(skill_file);
            free(skill_dir);
            free(dir);
            return 1;
        }
        printf("Overwriting existing skill '%s'\n", name);
    }
    if (!os_path_exists(skill_dir)) {
        os_mkdir_p(skill_dir);
    }

    FILE *fp = fopen(skill_file, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot write %s\n", skill_file);
        free(skill_file);
        free(skill_dir);
        free(dir);
        return 1;
    }
    fprintf(fp, "---\nname: %s\n", name);
    if (description && description[0])
        fprintf(fp, "description: %s\n", description);
    fprintf(fp, "---\n\n");
    if (instruction && instruction[0])
        fprintf(fp, "%s\n", instruction);
    else
        fprintf(fp, "You are the '%s' skill. "
                    "Describe what this skill does and how to use it.\n", name);
    fclose(fp);

    printf("Created skill '%s' at %s\n", name, skill_file);
    if (description && description[0])
        printf("  description: %s\n", description);
    printf("  trigger: /%s\n", name);

    free(skill_file);
    free(skill_dir);
    free(dir);
    return 0;
}

/* ── remove ─────────────────────────────────────────────────────── */

static int skill_cli_remove(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent skill remove <name>\n");
        return 1;
    }
    const char *name = argv[0];
    if (!valid_skill_name(name)) {
        fprintf(stderr, "Error: invalid skill name '%s'\n", name);
        return 1;
    }

    char *dir = skills_dir_path();
    if (!dir) {
        fprintf(stderr, "Error: cannot resolve skills directory\n");
        return 1;
    }
    char *skill_dir = os_path_join(dir, name);
    char *skill_file = skill_dir ? os_path_join(skill_dir, "SKILL.md") : NULL;
    if (!skill_dir || !skill_file) {
        fprintf(stderr, "Error: out of memory\n");
        free(skill_file);
        free(skill_dir);
        free(dir);
        return 1;
    }

    if (!os_path_exists(skill_file)) {
        fprintf(stderr, "Skill '%s' not found in %s\n", name, dir);
        free(skill_file);
        free(skill_dir);
        free(dir);
        return 1;
    }
    if (remove_recursive(skill_dir) != 0) {
        fprintf(stderr, "Error: failed to remove %s\n", skill_dir);
        free(skill_file);
        free(skill_dir);
        free(dir);
        return 1;
    }
    printf("Removed skill '%s' from %s\n", name, dir);
    free(skill_file);
    free(skill_dir);
    free(dir);
    return 0;
}

/* ── show ───────────────────────────────────────────────────────── */

static int skill_cli_show(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent skill show <name>\n");
        return 1;
    }
    const char *name = argv[0];
    char *dir = skills_dir_path();
    if (!dir) {
        fprintf(stderr, "Error: cannot resolve skills directory\n");
        return 1;
    }
    skill_list_t *list = skills_load_directory(dir);
    free(dir);
    if (!list) {
        fprintf(stderr, "Error: failed to load skills\n");
        return 1;
    }
    skill_t *s = skills_find(list, name);
    if (!s) {
        fprintf(stderr, "Skill '%s' not found\n", name);
        skills_free(list);
        return 1;
    }

    printf("Name:        %s\n", s->name);
    printf("Description: %s\n",
           s->description && s->description[0] ? s->description : "(none)");
    printf("Trigger:     %s\n", s->trigger ? s->trigger : "");
    printf("Path:        %s\n", s->path ? s->path : "");
    printf("\nInstruction:\n%s\n", s->instruction ? s->instruction : "");
    skills_free(list);
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────── */

int skill_main(int argc, char **argv) {
    if (argc < 2) {
        skill_usage();
        return 1;
    }
    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        skill_usage();
        return 0;
    }
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
        return skill_cli_list();
    if (strcmp(cmd, "add") == 0 || strcmp(cmd, "new") == 0 ||
        strcmp(cmd, "create") == 0)
        return skill_cli_add(rest_argc, rest_argv);
    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0 ||
        strcmp(cmd, "delete") == 0)
        return skill_cli_remove(rest_argc, rest_argv);
    if (strcmp(cmd, "show") == 0 || strcmp(cmd, "info") == 0)
        return skill_cli_show(rest_argc, rest_argv);

    fprintf(stderr, "Unknown skill command: %s\n\n", cmd);
    skill_usage();
    return 1;
}
