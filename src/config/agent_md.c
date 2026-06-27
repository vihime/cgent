/*
 * agent_md.c — agent.md YAML frontmatter parser
 *
 * Parses markdown files with YAML frontmatter:
 * ---
 * name: my-agent
 * description: A helper
 * model: deepseek-chat
 * mcp_servers:
 *   - filesystem
 * skills:
 *   - code-review
 * ---
 * Instruction text here...
 */
#include "config.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Minimal YAML frontmatter parser — handles the subset we need:
 *   - key: value
 *   - key:
 *       - item
 *       - item
 *   - nested:
 *       key: value
 */

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
} yaml_reader_t;

__attribute__((unused))
static void skip_ws(yaml_reader_t *r) {
    while (r->pos < r->len && (r->data[r->pos] == ' ' || r->data[r->pos] == '\t'))
        r->pos++;
}

static char *read_line(yaml_reader_t *r) {
    if (r->pos >= r->len) return NULL;
    size_t start = r->pos;
    while (r->pos < r->len && r->data[r->pos] != '\n' && r->data[r->pos] != '\r')
        r->pos++;
    size_t end = r->pos;
    /* Skip newline */
    if (r->pos < r->len && r->data[r->pos] == '\r') r->pos++;
    if (r->pos < r->len && r->data[r->pos] == '\n') r->pos++;
    /* Trim trailing whitespace */
    while (end > start && (r->data[end-1] == ' ' || r->data[end-1] == '\t'))
        end--;
    if (end == start) return strdup("");
    char *line = malloc(end - start + 1);
    memcpy(line, r->data + start, end - start);
    line[end - start] = '\0';
    return line;
}

__attribute__((unused))
static int get_indent(const char *line) {
    int n = 0;
    while (*line == ' ' || *line == '\t') { n++; line++; }
    return n;
}

static char *strip(char *s) {
    while (isspace(*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) { *end = '\0'; end--; }
    return s;
}

static char *strip_quotes(char *s) {
    s = strip(s);
    size_t len = strlen(s);
    if (len >= 2) {
        if ((s[0] == '"' && s[len-1] == '"') ||
            (s[0] == '\'' && s[len-1] == '\'')) {
            s[len-1] = '\0';
            return s + 1;
        }
    }
    return s;
}

agent_md_t *agent_md_parse(const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return NULL;

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); return NULL; }

    char *data = malloc(fsize + 1);
    if (!data) { fclose(fp); return NULL; }
    size_t nread = fread(data, 1, fsize, fp);
    data[nread] = '\0';
    fclose(fp);

    agent_md_t *am = calloc(1, sizeof(agent_md_t));
    if (!am) { free(data); return NULL; }

    /* Check for YAML frontmatter (must start with "---") */
    if (strncmp(data, "---", 3) != 0) {
        /* No frontmatter — treat entire file as instruction */
        am->instruction = strdup(data);
        free(data);
        return am;
    }

    /* Find closing "---" */
    char *end_fm = strstr(data + 3, "\n---");
    char *body_start;
    if (end_fm) {
        body_start = end_fm + 4; /* past \n--- */
        /* Skip trailing newline after --- */
        if (*body_start == '\n') body_start++;
        else if (*body_start == '\r' && body_start[1] == '\n') body_start += 2;
    } else {
        /* No closing ---, treat whole thing as instruction */
        am->instruction = strdup(data);
        free(data);
        return am;
    }

    /* Parse frontmatter */
    yaml_reader_t r = { .data = data + 3, .len = (size_t)(end_fm - data), .pos = 0 };

    /* Skip opening newline after first --- */
    while (r.pos < r.len && (r.data[r.pos] == '\n' || r.data[r.pos] == '\r'))
        r.pos++;

    char ***current_list_ptr = NULL;  /* points to am->skills or am->mcp_servers */
    int *current_count = NULL;       /* points to the corresponding count */

    while (r.pos < r.len) {
        char *line = read_line(&r);
        if (!line) break;
        char *s = strip(line);
        if (*s == '\0' || *s == '#') { free(line); continue; }

        /* Check if this is a list item */
        if (s[0] == '-' && (s[1] == ' ' || s[1] == '\t')) {
            if (current_list_ptr && current_count) {
                char *item = strip(s + 1);
                item = strip_quotes(item);
                *current_list_ptr = realloc(*current_list_ptr,
                    (*current_count + 1) * sizeof(char *));
                (*current_list_ptr)[*current_count] = strdup(item);
                (*current_count)++;
            }
            free(line);
            continue;
        }

        /* Key: value or Key: */
        char *colon = strchr(s, ':');
        if (!colon) { free(line); continue; }

        *colon = '\0';
        char *key = strip(s);
        char *value = strip(colon + 1);

        /* End previous list context */
        current_list_ptr = NULL;
        current_count = NULL;

        if (strcmp(key, "name") == 0) {
            value = strip_quotes(value);
            if (*value) am->name = strdup(value);
        } else if (strcmp(key, "description") == 0) {
            value = strip_quotes(value);
            if (*value) am->description = strdup(value);
        } else if (strcmp(key, "model") == 0) {
            value = strip_quotes(value);
            if (*value) am->model = strdup(value);
        } else if (strcmp(key, "mcp_servers") == 0 || strcmp(key, "mcp-servers") == 0) {
            if (*value == '\0') {
                current_list_ptr = &am->mcp_servers;
                current_count = &am->mcp_servers_count;
            } else {
                value = strip_quotes(value);
                am->mcp_servers = realloc(am->mcp_servers,
                    (am->mcp_servers_count + 1) * sizeof(char *));
                am->mcp_servers[am->mcp_servers_count++] = strdup(value);
            }
        } else if (strcmp(key, "skills") == 0) {
            if (*value == '\0') {
                current_list_ptr = &am->skills;
                current_count = &am->skills_count;
            } else {
                value = strip_quotes(value);
                am->skills = realloc(am->skills,
                    (am->skills_count + 1) * sizeof(char *));
                am->skills[am->skills_count++] = strdup(value);
            }
        }

        free(line);
    }

    /* Parse body (instruction) */
    if (body_start && *body_start) {
        am->instruction = strdup(body_start);
    }

    free(data);
    return am;
}

void agent_md_free(agent_md_t *am) {
    if (!am) return;
    free(am->name);
    free(am->description);
    free(am->model);
    free(am->instruction);
    for (int i = 0; i < am->mcp_servers_count; i++)
        free(am->mcp_servers[i]);
    free(am->mcp_servers);
    for (int i = 0; i < am->skills_count; i++)
        free(am->skills[i]);
    free(am->skills);
    free(am);
}
