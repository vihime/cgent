/*
 * builtin_tools.c — Built-in tools (read_file, write_file, edit, bash, think,
 *                   glob, grep, web_fetch, web_search, mailbox, list_dir,
 *                   apply_patch, git_status, git_diff, git_log, subagent)
 */
#include "tools.h"
#include "json.h"
#include "platform.h"
#include "subagent.h"
#include "mailbox.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Cap on bytes returned by read_file — protects the model context. */
#define READ_FILE_CAP (256 * 1024)

/* ── Shell quoting helper ───────────────────────────────────────── */

/* Wrap a string in single quotes for safe embedding in a shell command.
 * Embedded single quotes are escaped as '\''. Returns malloc'd string. */
static char *shell_quote(const char *s) {
    if (!s) return strdup("''");
    char *out = malloc(strlen(s) * 4 + 3);
    if (!out) return NULL;
    size_t o = 0;
    out[o++] = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    out[o++] = '\'';
    out[o] = '\0';
    return out;
}

/* ── read_file ──────────────────────────────────────────────────── */

static char *tool_read_file(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *path_val = json_object_get(args, "path");
    if (!path_val || !json_is_string(path_val)) {
        if (error) *error = strdup("Missing 'path' argument");
        json_free(args);
        return NULL;
    }

    const char *path = json_string_value(path_val);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Cannot read file: %s", path);
        if (error) *error = strdup(buf);
        json_free(args);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    long to_read = size > READ_FILE_CAP ? READ_FILE_CAP : size;
    char *content = malloc(to_read + 256);
    if (!content) {
        fclose(fp);
        json_free(args);
        if (error) *error = strdup("Memory allocation failed");
        return NULL;
    }

    size_t nread = fread(content, 1, to_read, fp);
    if (size > to_read) {
        int olen = snprintf(content + nread, 256,
            "\n... (file is %ld bytes, showing first %d)\n", size, READ_FILE_CAP);
        nread += olen > 0 ? (size_t)olen : 0;
    }
    content[nread] = '\0';
    fclose(fp);
    json_free(args);

    return content;
}

/* ── write_file ─────────────────────────────────────────────────── */

static char *tool_write_file(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *path_val = json_object_get(args, "path");
    json_value_t *content_val = json_object_get(args, "content");

    if (!path_val || !json_is_string(path_val)) {
        if (error) *error = strdup("Missing 'path' argument");
        json_free(args);
        return NULL;
    }
    if (!content_val || !json_is_string(content_val)) {
        if (error) *error = strdup("Missing 'content' argument");
        json_free(args);
        return NULL;
    }

    const char *path = json_string_value(path_val);
    const char *content = json_string_value(content_val);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Cannot write file: %s", path);
        if (error) *error = strdup(buf);
        json_free(args);
        return NULL;
    }

    fputs(content, fp);
    fclose(fp);
    json_free(args);

    return strdup("File written successfully");
}

/* ── bash ───────────────────────────────────────────────────────── */

static char *tool_bash(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *cmd_val = json_object_get(args, "command");
    if (!cmd_val || !json_is_string(cmd_val)) {
        if (error) *error = strdup("Missing 'command' argument");
        json_free(args);
        return NULL;
    }

    const char *command = json_string_value(cmd_val);
    int exit_code;
    char *output = os_exec_capture_timeout(command, 30000, &exit_code);
    json_free(args);

    if (!output) {
        if (error) *error = strdup("Command execution failed or timed out");
        return NULL;
    }

    /* Return JSON with output and exit code */
    json_value_t *result = json_object();
    json_object_set(result, "stdout", json_string(output));
    json_object_set(result, "exit_code", json_number(exit_code));
    free(output);

    char *result_str = json_stringify(result);
    json_free(result);
    return result_str;
}

/* ── think ──────────────────────────────────────────────────────── */

static char *tool_think(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *thought_val = json_object_get(args, "thought");
    const char *thought = thought_val && json_is_string(thought_val)
        ? json_string_value(thought_val) : "(no thought provided)";

    /* think is a no-op — just acknowledges the thought */
    json_value_t *result = json_object();
    json_object_set(result, "acknowledged", json_bool(true));
    json_object_set(result, "thought", json_string(thought));

    char *result_str = json_stringify(result);
    json_free(result);
    json_free(args);
    return result_str;
}

/* ── spawn_subagent ─────────────────────────────────────────────── */

static char *tool_spawn_subagent(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *task_val = json_object_get(args, "task");
    json_value_t *model_val = json_object_get(args, "model");
    json_value_t *provider_val = json_object_get(args, "provider");
    json_value_t *prompt_val = json_object_get(args, "system_prompt");

    if (!task_val || !json_is_string(task_val)) {
        if (error) *error = strdup("Missing 'task' argument");
        json_free(args);
        return NULL;
    }

    subagent_config_t cfg = {
        .provider     = (char *)(provider_val ? json_string_value(provider_val) : "deepseek"),
        .model        = (char *)(model_val ? json_string_value(model_val) : "deepseek-v4-flash"),
        .api_key      = NULL,
        .system_prompt = (char *)(prompt_val ? json_string_value(prompt_val) : NULL),
        .task         = (char *)json_string_value(task_val),
        .temperature  = 0.0,
        .max_tokens   = 2048,
        .timeout_seconds = 120,
    };

    /* Resolve API key from environment (provider-specific) */
    if (!cfg.api_key) cfg.api_key = os_getenv("CGENT_API_KEY");

    subagent_result_t *result = subagent_run(&cfg);
    free(cfg.api_key);

    if (!result) {
        if (error) *error = strdup("Failed to spawn subagent");
        json_free(args);
        return NULL;
    }

    /* Build result JSON */
    json_value_t *out = json_object();
    if (result->output) {
        json_object_set(out, "output", json_string(result->output));
    }
    if (result->error) {
        json_object_set(out, "error", json_string(result->error));
    }
    json_object_set(out, "timed_out", json_bool(result->timed_out));
    json_object_set(out, "wall_time", json_number(result->wall_time_seconds));

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    subagent_result_free(result);
    return out_str;
}

/* ── edit ─────────────────────────────────────────────────────── */

static char *tool_edit(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *path_val   = json_object_get(args, "file_path");
    json_value_t *old_val    = json_object_get(args, "old_string");
    json_value_t *new_val    = json_object_get(args, "new_string");

    if (!path_val || !json_is_string(path_val)) {
        if (error) *error = strdup("Missing 'file_path' argument");
        json_free(args); return NULL;
    }
    if (!old_val || !json_is_string(old_val)) {
        if (error) *error = strdup("Missing 'old_string' argument");
        json_free(args); return NULL;
    }
    if (!new_val || !json_is_string(new_val)) {
        if (error) *error = strdup("Missing 'new_string' argument");
        json_free(args); return NULL;
    }

    const char *path    = json_string_value(path_val);
    const char *old_str = json_string_value(old_val);
    const char *new_str = json_string_value(new_val);

    /* Read the file */
    FILE *fp = fopen(path, "r");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Cannot read file: %s", path);
        if (error) *error = strdup(buf);
        json_free(args); return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(sz + 1);
    if (!content) { fclose(fp); json_free(args); return NULL; }
    size_t nread = fread(content, 1, sz, fp);
    content[nread] = '\0';
    fclose(fp);

    /* Find old_string */
    char *found = strstr(content, old_str);
    if (!found) {
        free(content);
        if (error) *error = strdup("old_string not found in file");
        json_free(args); return NULL;
    }

    /* Check uniqueness */
    if (strstr(found + strlen(old_str), old_str)) {
        free(content);
        if (error) *error = strdup("old_string is not unique in file");
        json_free(args); return NULL;
    }

    /* Build replacement */
    size_t new_len = strlen(new_str);
    size_t old_len = strlen(old_str);
    size_t prefix  = found - content;
    size_t suffix  = nread - prefix - old_len;
    size_t total   = prefix + new_len + suffix;

    char *result = malloc(total + 1);
    if (!result) { free(content); json_free(args); return NULL; }
    memcpy(result, content, prefix);
    memcpy(result + prefix, new_str, new_len);
    memcpy(result + prefix + new_len, found + old_len, suffix);
    result[total] = '\0';
    free(content);

    /* Write back */
    fp = fopen(path, "w");
    if (!fp) {
        free(result);
        if (error) *error = strdup("Cannot write file");
        json_free(args); return NULL;
    }
    fputs(result, fp);
    fclose(fp);
    free(result);

    json_value_t *out = json_object();
    json_object_set(out, "success", json_bool(true));
    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── glob ─────────────────────────────────────────────────────── */

static char *tool_glob(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *pattern_val = json_object_get(args, "pattern");
    if (!pattern_val || !json_is_string(pattern_val)) {
        if (error) *error = strdup("Missing 'pattern' argument");
        json_free(args); return NULL;
    }

    const char *pattern = json_string_value(pattern_val);

    /* Use find to locate files matching pattern */
    char *qpat = shell_quote(pattern);
    if (!qpat) {
        if (error) *error = strdup("Memory allocation failed");
        json_free(args); return NULL;
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "find . -path './.git' -prune -o -path './third_party' -prune "
             "-o -name %s -print 2>/dev/null | head -200", qpat);
    free(qpat);

    int exit_code;
    char *output = os_exec_capture_timeout(cmd, 15000, &exit_code);

    json_value_t *out = json_object();
    json_value_t *files = json_array();

    if (output && output[0]) {
        /* Split output by newlines */
        char *saveptr;
        char *line = strtok_r(output, "\n", &saveptr);
        while (line) {
            /* Strip leading ./ */
            if (strncmp(line, "./", 2) == 0) line += 2;
            if (line[0]) json_array_append(files, json_string(line));
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    free(output);

    json_object_set(out, "files", files);
    json_object_set(out, "count", json_number(json_array_length(files)));

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── grep ─────────────────────────────────────────────────────── */

static char *tool_grep(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *pattern_val = json_object_get(args, "pattern");
    if (!pattern_val || !json_is_string(pattern_val)) {
        if (error) *error = strdup("Missing 'pattern' argument");
        json_free(args); return NULL;
    }

    const char *pattern  = json_string_value(pattern_val);
    const char *include  = NULL;
    json_value_t *inc_val = json_object_get(args, "include");
    if (inc_val && json_is_string(inc_val)) include = json_string_value(inc_val);

    /* Build grep command — both pattern and include are shell-quoted */
    char *qpat = shell_quote(pattern);
    if (!qpat) {
        if (error) *error = strdup("Memory allocation failed");
        json_free(args); return NULL;
    }
    char *qinc = include ? shell_quote(include) : NULL;
    char cmd[4096];
    if (qinc) {
        snprintf(cmd, sizeof(cmd),
                 "grep -rn --include=%s %s . "
                 "--exclude-dir=.git --exclude-dir=third_party "
                 "--exclude-dir=.cgent 2>/dev/null | head -200",
                 qinc, qpat);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "grep -rn %s . "
                 "--exclude-dir=.git --exclude-dir=third_party "
                 "--exclude-dir=.cgent 2>/dev/null | head -200",
                 qpat);
    }
    free(qpat);
    free(qinc);

    int exit_code;
    char *output = os_exec_capture_timeout(cmd, 15000, &exit_code);

    json_value_t *out = json_object();
    json_value_t *matches = json_array();

    if (output && output[0]) {
        char *saveptr;
        char *line = strtok_r(output, "\n", &saveptr);
        while (line) {
            /* Strip leading ./ */
            if (strncmp(line, "./", 2) == 0) line += 2;
            if (line[0]) {
                /* Parse file:line:content */
                json_value_t *m = json_object();
                char *colon1 = strchr(line, ':');
                if (colon1) {
                    *colon1 = '\0';
                    json_object_set(m, "file", json_string(line));
                    char *colon2 = strchr(colon1 + 1, ':');
                    if (colon2) {
                        *colon2 = '\0';
                        json_object_set(m, "line", json_number(atoi(colon1 + 1)));
                        json_object_set(m, "content", json_string(colon2 + 1));
                    }
                    *colon1 = ':';
                    if (colon2) *colon2 = ':';
                }
                json_array_append(matches, m);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    free(output);

    json_object_set(out, "matches", matches);
    json_object_set(out, "count", json_number(json_array_length(matches)));

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── web_fetch ─────────────────────────────────────────────────── */

static char *tool_web_fetch(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *url_val = json_object_get(args, "url");
    if (!url_val || !json_is_string(url_val)) {
        if (error) *error = strdup("Missing 'url' argument");
        json_free(args); return NULL;
    }

    const char *url = json_string_value(url_val);

    /* Only allow http/https — prevents shell metacharacter abuse via the URL */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        if (error) *error = strdup("Only http:// and https:// URLs are allowed");
        json_free(args);
        return NULL;
    }

    /* Use curl to fetch the URL */
    char *qurl = shell_quote(url);
    if (!qurl) {
        if (error) *error = strdup("Memory allocation failed");
        json_free(args);
        return NULL;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 30 --connect-timeout 10 "
             "-H 'User-Agent: cgent/0.1' %s 2>/dev/null | head -c 65536",
             qurl);
    free(qurl);

    int exit_code;
    char *output = os_exec_capture_timeout(cmd, 30000, &exit_code);

    json_value_t *out = json_object();
    if (output && output[0]) {
        json_object_set(out, "content", json_string(output));
        json_object_set(out, "length", json_number(strlen(output)));
    } else {
        json_object_set(out, "content", json_string(""));
        json_object_set(out, "length", json_number(0));
        json_object_set(out, "error", json_string("Failed to fetch URL"));
    }
    json_object_set(out, "status_code", json_number(exit_code == 0 ? 200 : 0));

    free(output);

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── web_search ─────────────────────────────────────────────────── */

static char *tool_web_search(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *query_val = json_object_get(args, "query");
    if (!query_val || !json_is_string(query_val)) {
        if (error) *error = strdup("Missing 'query' argument");
        json_free(args); return NULL;
    }

    const char *query = json_string_value(query_val);

    /* Use curl to fetch search results from DuckDuckGo HTML.
     * --data-urlencode lets curl do the encoding, and the query is
     * shell-quoted so it can't break out of the command. */
    char *qq = shell_quote(query);
    if (!qq) {
        if (error) *error = strdup("Memory allocation failed");
        json_free(args);
        return NULL;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 30 --connect-timeout 10 "
             "-H 'User-Agent: cgent/0.1' "
             "-G 'https://html.duckduckgo.com/html/' --data-urlencode q=%s "
             "2>/dev/null | "
             "grep -oE 'class=\"result__snippet\">[^<]+' | "
             "sed 's/class=\"result__snippet\">//' | head -20",
             qq);
    free(qq);

    int exit_code;
    char *output = os_exec_capture_timeout(cmd, 30000, &exit_code);

    json_value_t *out = json_object();
    json_value_t *results = json_array();

    if (output && output[0]) {
        char *saveptr;
        char *line = strtok_r(output, "\n", &saveptr);
        while (line) {
            while (*line == ' ') line++;
            if (line[0]) json_array_append(results, json_string(line));
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    free(output);

    json_object_set(out, "results", results);
    json_object_set(out, "count", json_number(json_array_length(results)));

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── list_dir ──────────────────────────────────────────────────── */

typedef struct {
    char *name;
    char type;          /* 'd' directory, 'f' file, 'l' symlink, '?' other */
    long long size;
} list_entry_t;

static int list_entry_cmp(const void *a, const void *b) {
    const list_entry_t *ea = (const list_entry_t *)a;
    const list_entry_t *eb = (const list_entry_t *)b;
    if (ea->type == 'd' && eb->type != 'd') return -1;
    if (ea->type != 'd' && eb->type == 'd') return 1;
    return strcmp(ea->name, eb->name);
}

static char *tool_list_dir(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *path_val = json_object_get(args, "path");
    const char *path = path_val && json_is_string(path_val)
        ? json_string_value(path_val) : ".";
    json_value_t *all_val = json_object_get(args, "all");
    bool show_all = all_val && json_is_bool(all_val) && json_bool_value(all_val);

    DIR *d = opendir(path);
    if (!d) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Cannot open directory: %s", path);
        if (error) *error = strdup(buf);
        json_free(args);
        return NULL;
    }

    list_entry_t *entries = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 20000) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!show_all && e->d_name[0] == '.') continue;

        char *full = os_path_join(path, e->d_name);
        struct stat st;
        char type = '?';
        long long sz = 0;
        if (full && stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) type = 'd';
            else if (S_ISREG(st.st_mode)) type = 'f';
            else if (S_ISLNK(st.st_mode)) type = 'l';
            sz = (long long)st.st_size;
        }
        free(full);

        if (count >= cap) {
            cap = cap ? cap * 2 : 64;
            list_entry_t *grown = realloc(entries, cap * sizeof(list_entry_t));
            if (!grown) {
                for (int i = 0; i < count; i++) free(entries[i].name);
                free(entries);
                closedir(d);
                if (error) *error = strdup("Memory allocation failed");
                json_free(args);
                return NULL;
            }
            entries = grown;
        }
        entries[count].name = strdup(e->d_name);
        entries[count].type = type;
        entries[count].size = sz;
        count++;
    }
    closedir(d);

    if (count > 0)
        qsort(entries, count, sizeof(list_entry_t), list_entry_cmp);

    json_value_t *out = json_object();
    json_value_t *arr = json_array();
    for (int i = 0; i < count; i++) {
        json_value_t *j = json_object();
        json_object_set(j, "name", json_string(entries[i].name));
        char tbuf[2] = { entries[i].type, '\0' };
        json_object_set(j, "type", json_string(tbuf));
        json_object_set(j, "size", json_number((double)entries[i].size));
        json_array_append(arr, j);
        free(entries[i].name);
    }
    free(entries);
    json_object_set(out, "entries", arr);
    json_object_set(out, "count", json_number(count));

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── apply_patch ───────────────────────────────────────────────── */

typedef struct {
    int old_start;      /* 1-based line in the original file */
    int old_count;
    int new_start;
    int new_count;
    char **lines;       /* hunk body lines including the prefix char */
    int n_lines;
} patch_hunk_t;

static void patch_hunk_free(patch_hunk_t *h) {
    if (!h) return;
    for (int i = 0; i < h->n_lines; i++) free(h->lines[i]);
    free(h->lines);
    h->lines = NULL;
    h->n_lines = 0;
}

static void patch_hunk_counts(const patch_hunk_t *h, int *old_n, int *new_n) {
    int o = 0, n = 0;
    for (int i = 0; i < h->n_lines; i++) {
        if (h->lines[i][0] == ' ') { o++; n++; }
        else if (h->lines[i][0] == '-') o++;
        else if (h->lines[i][0] == '+') n++;
    }
    *old_n = o;
    *new_n = n;
}

static bool patch_hunk_matches(char **lines, int n_lines,
                               const patch_hunk_t *h, int idx) {
    int k = 0;
    for (int i = 0; i < h->n_lines; i++) {
        char p = h->lines[i][0];
        if (p == ' ' || p == '-') {
            if (idx + k >= n_lines) return false;
            if (strcmp(h->lines[i] + 1, lines[idx + k]) != 0) return false;
            k++;
        }
        /* '+' and '\' lines are not matched against the file */
    }
    return true;
}

static int patch_apply_hunk(char ***lines_ptr, int *n_lines_ptr,
                            const patch_hunk_t *h, int *offset) {
    char **lines = *lines_ptr;
    int n_lines = *n_lines_ptr;
    int base = h->old_start - 1 + *offset;

    /* Exact position first, then fuzz up to 4 lines */
    int idx = -1;
    for (int d = 0; d <= 4 && idx < 0; d++) {
        if (base - d >= 0 && patch_hunk_matches(lines, n_lines, h, base - d))
            idx = base - d;
        else if (patch_hunk_matches(lines, n_lines, h, base + d))
            idx = base + d;
    }
    if (idx < 0) return -1;

    int old_n, new_n;
    patch_hunk_counts(h, &old_n, &new_n);
    int new_count = n_lines - old_n + new_n;
    char **new_lines = calloc(new_count ? new_count : 1, sizeof(char *));
    if (!new_lines) return -1;

    int out = 0;
    for (int i = 0; i < idx; i++) new_lines[out++] = lines[i];
    for (int i = 0; i < h->n_lines; i++) {
        char p = h->lines[i][0];
        if (p == ' ' || p == '+') new_lines[out++] = strdup(h->lines[i] + 1);
        /* '-' lines are dropped */
    }
    for (int i = idx + old_n; i < n_lines; i++) new_lines[out++] = lines[i];

    free(lines);
    *lines_ptr = new_lines;
    *n_lines_ptr = out;
    *offset += new_n - old_n;
    return 0;
}

/* Apply a set of hunks to a single file.
 *   add: file is new (--- /dev/null)
 *   del: file is deleted (+++ /dev/null)
 * Returns 0 on success, -1 on error (with *err_msg set). */
static int patch_apply_file(const char *target, bool add, bool del,
                            patch_hunk_t *hunks, int n_hunks,
                            int *hunks_applied, char **err_msg) {
    if (del) {
        if (remove(target) != 0) {
            if (err_msg) {
                char buf[512];
                snprintf(buf, sizeof(buf), "Cannot delete file: %s", target);
                *err_msg = strdup(buf);
            }
            return -1;
        }
        if (hunks_applied) (*hunks_applied)++;
        return 0;
    }

    if (add) {
        if (os_path_exists(target)) {
            if (err_msg) {
                char buf[512];
                snprintf(buf, sizeof(buf), "File already exists: %s", target);
                *err_msg = strdup(buf);
            }
            return -1;
        }
        FILE *fp = fopen(target, "w");
        if (!fp) {
            if (err_msg) {
                char buf[512];
                snprintf(buf, sizeof(buf), "Cannot create file: %s", target);
                *err_msg = strdup(buf);
            }
            return -1;
        }
        for (int h = 0; h < n_hunks; h++) {
            for (int i = 0; i < hunks[h].n_lines; i++) {
                if (hunks[h].lines[i][0] == '+') {
                    fputs(hunks[h].lines[i] + 1, fp);
                    fputc('\n', fp);
                }
            }
        }
        fclose(fp);
        if (hunks_applied) (*hunks_applied)++;
        return 0;
    }

    /* Update existing file */
    FILE *fp = fopen(target, "r");
    if (!fp) {
        if (err_msg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "Cannot read file: %s", target);
            *err_msg = strdup(buf);
        }
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz > 8 * 1024 * 1024) {
        fclose(fp);
        if (err_msg) *err_msg = strdup("Patch target too large (over 8 MB)");
        return -1;
    }
    char *content = malloc(sz + 1);
    if (!content) { fclose(fp); if (err_msg) *err_msg = strdup("Out of memory"); return -1; }
    size_t nread = fread(content, 1, sz, fp);
    content[nread] = '\0';
    fclose(fp);
    bool trailing_nl = nread > 0 && content[nread - 1] == '\n';

    /* Split into lines (without newlines), preserving empty lines */
    char **lines = NULL;
    int n_lines = 0;
    {
        const char *p = content;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t llen = nl ? (size_t)(nl - p) : strlen(p);
            char **grown = realloc(lines, (n_lines + 1) * sizeof(char *));
            if (!grown) {
                for (int i = 0; i < n_lines; i++) free(lines[i]);
                free(lines); free(content);
                if (err_msg) *err_msg = strdup("Out of memory");
                return -1;
            }
            lines = grown;
            lines[n_lines] = strndup(p, llen);
            n_lines++;
            if (!nl) break;
            p = nl + 1;
        }
    }
    free(content);

    int offset = 0;
    for (int h = 0; h < n_hunks; h++) {
        if (patch_apply_hunk(&lines, &n_lines, &hunks[h], &offset) != 0) {
            for (int i = 0; i < n_lines; i++) free(lines[i]);
            free(lines);
            if (err_msg) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "Patch hunk %d does not match %s (context mismatch)",
                         h + 1, target);
                *err_msg = strdup(buf);
            }
            return -1;
        }
        if (hunks_applied) (*hunks_applied)++;
    }

    fp = fopen(target, "w");
    if (!fp) {
        for (int i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        if (err_msg) {
            char buf[512];
            snprintf(buf, sizeof(buf), "Cannot write file: %s", target);
            *err_msg = strdup(buf);
        }
        return -1;
    }
    for (int i = 0; i < n_lines; i++) {
        fputs(lines[i], fp);
        if (i < n_lines - 1 || trailing_nl) fputc('\n', fp);
    }
    fclose(fp);
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    free(lines);
    return 0;
}

static char *tool_apply_patch(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *patch_val = json_object_get(args, "patch");
    if (!patch_val || !json_is_string(patch_val)) {
        if (error) *error = strdup("Missing 'patch' argument");
        json_free(args);
        return NULL;
    }
    const char *patch = json_string_value(patch_val);
    json_value_t *path_val = json_object_get(args, "path");
    const char *path_override = path_val && json_is_string(path_val)
        ? json_string_value(path_val) : NULL;

    /* Split the patch text into lines */
    char **plines = NULL;
    int n = 0;
    {
        const char *p = patch;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t llen = nl ? (size_t)(nl - p) : strlen(p);
            char **grown = realloc(plines, (n + 1) * sizeof(char *));
            if (!grown) {
                for (int i = 0; i < n; i++) free(plines[i]);
                free(plines);
                if (error) *error = strdup("Out of memory");
                json_free(args);
                return NULL;
            }
            plines = grown;
            plines[n] = strndup(p, llen);
            n++;
            if (!nl) break;
            p = nl + 1;
        }
    }

    json_value_t *out = json_object();
    int applied_files = 0;
    int hunks_applied = 0;
    int i = 0;
    bool ok = true;

    while (i < n) {
        /* Find next file section: "--- " header */
        while (i < n && strncmp(plines[i], "--- ", 4) != 0) i++;
        if (i >= n) break;

        char *old_path = plines[i] + 4;
        i++;
        if (i >= n || strncmp(plines[i], "+++ ", 4) != 0) {
            if (error) *error = strdup("Malformed patch: missing '+++' header");
            ok = false;
            break;
        }
        char *new_path = plines[i] + 4;
        i++;

        /* Trim trailing whitespace from both paths */
        {
            char *t = old_path + strlen(old_path);
            while (t > old_path && (t[-1] == ' ' || t[-1] == '\r')) *--t = '\0';
            t = new_path + strlen(new_path);
            while (t > new_path && (t[-1] == ' ' || t[-1] == '\r')) *--t = '\0';
        }

        bool add = strcmp(old_path, "/dev/null") == 0;
        bool del = strcmp(new_path, "/dev/null") == 0;

        const char *target = path_override;
        if (!target) {
            if (add) target = new_path;
            else if (del) target = old_path;
            else target = new_path;
        }
        /* Strip common a/ b/ prefixes */
        if (strncmp(target, "a/", 2) == 0 || strncmp(target, "b/", 2) == 0)
            target += 2;

        /* Collect hunks for this file */
        patch_hunk_t *hunks = NULL;
        int n_hunks = 0;
        patch_hunk_t cur = {0};
        bool have_hunk = false;

        while (i < n && strncmp(plines[i], "--- ", 4) != 0) {
            const char *line = plines[i];
            if (strncmp(line, "@@ ", 3) == 0) {
                /* Finalize previous hunk */
                if (have_hunk) {
                    hunks = realloc(hunks, (n_hunks + 1) * sizeof(patch_hunk_t));
                    if (hunks) hunks[n_hunks++] = cur;
                    else { cur.lines = NULL; ok = false; break; }
                    cur.lines = NULL; cur.n_lines = 0;
                }
                int a1 = 0, a2 = 1, b1 = 0, b2 = 1;
                int parsed = sscanf(line, "@@ -%d,%d +%d,%d @@",
                                    &a1, &a2, &b1, &b2);
                if (parsed < 4) {
                    a2 = 1; b2 = 1;
                    parsed = sscanf(line, "@@ -%d +%d @@", &a1, &b1);
                }
                if (parsed < 2) {
                    if (error) *error = strdup("Malformed patch: bad @@ header");
                    ok = false;
                    break;
                }
                cur.old_start = a1;
                cur.old_count = a2;
                cur.new_start = b1;
                cur.new_count = b2;
                have_hunk = true;
            } else if (have_hunk && line[0] != '\0') {
                /* Body line — skip "\ No newline at end of file" markers */
                if (line[0] == '\\') { i++; continue; }
                char **grown = realloc(cur.lines,
                                       (cur.n_lines + 1) * sizeof(char *));
                if (!grown) { ok = false; break; }
                cur.lines = grown;
                cur.lines[cur.n_lines++] = strdup(line);
            } else if (!have_hunk && line[0] != '\0' && line[0] != '\\') {
                /* Stray line before any hunk header */
                if (error) *error = strdup("Malformed patch: hunk body before @@ header");
                ok = false;
                break;
            }
            i++;
            if (!ok) break;
        }

        if (have_hunk && ok) {
            hunks = realloc(hunks, (n_hunks + 1) * sizeof(patch_hunk_t));
            if (hunks) hunks[n_hunks++] = cur;
            else { ok = false; }
            cur.lines = NULL; cur.n_lines = 0;
        }
        patch_hunk_free(&cur);

        if (!ok) {
            for (int h = 0; h < n_hunks; h++) patch_hunk_free(&hunks[h]);
            free(hunks);
            break;
        }

        if (n_hunks == 0) {
            if (error) *error = strdup("Malformed patch: no hunks for file section");
            ok = false;
            break;
        }

        char *err_msg = NULL;
        int applied = 0;
        if (patch_apply_file(target, add, del, hunks, n_hunks,
                             &applied, &err_msg) != 0) {
            json_object_set(out, "error",
                json_string(err_msg ? err_msg : "Failed to apply patch"));
            free(err_msg);
            ok = false;
        } else {
            applied_files++;
            hunks_applied += applied;
        }

        for (int h = 0; h < n_hunks; h++) patch_hunk_free(&hunks[h]);
        free(hunks);
        if (!ok) break;
    }

    for (int j = 0; j < n; j++) free(plines[j]);
    free(plines);

    if (ok) {
        json_object_set(out, "success", json_bool(true));
        json_object_set(out, "files", json_number(applied_files));
        json_object_set(out, "hunks_applied", json_number(hunks_applied));
    } else {
        json_object_set(out, "success", json_bool(false));
    }

    char *out_str = json_stringify(out);
    json_free(out);
    json_free(args);
    return out_str;
}

/* ── git tools ─────────────────────────────────────────────────── */

static char *git_run(const char *cmd, int *exit_code) {
    return os_exec_capture_timeout(cmd, 15000, exit_code);
}

static char *tool_git_status(const char *name, const char *args_json, char **error) {
    (void)name; (void)error;
    json_value_t *args = json_parse(args_json);
    if (args) json_free(args);

    int ec = 0;
    char *out = git_run("git status --porcelain=v1 2>&1", &ec);

    json_value_t *root = json_object();
    if (ec != 0 || !out) {
        json_object_set(root, "success", json_bool(false));
        json_object_set(root, "error",
            json_string(out && out[0] ? out : "not a git repository"));
        free(out);
        char *s = json_stringify(root);
        json_free(root);
        return s;
    }

    char *branch = NULL;
    {
        int bec = 0;
        char *b = git_run("git rev-parse --abbrev-ref HEAD 2>/dev/null", &bec);
        if (b && bec == 0) {
            size_t l = strlen(b);
            while (l > 0 && (b[l-1] == '\n' || b[l-1] == '\r')) b[--l] = '\0';
            branch = b;
        } else {
            free(b);
        }
    }
    json_object_set(root, "branch",
        branch && branch[0] ? json_string(branch) : json_string("(detached)"));
    free(branch);

    json_value_t *changes = json_array();
    if (out) {
        char *saveptr;
        char *line = strtok_r(out, "\n", &saveptr);
        while (line) {
            if (strlen(line) >= 4) {
                json_value_t *c = json_object();
                char idx[2] = { line[0], '\0' };
                char wt[2]  = { line[1], '\0' };
                json_object_set(c, "index", json_string(idx));
                json_object_set(c, "worktree", json_string(wt));
                json_object_set(c, "path", json_string(line + 3));
                json_array_append(changes, c);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    free(out);

    json_object_set(root, "success", json_bool(true));
    json_object_set(root, "changes", changes);
    json_object_set(root, "count", json_number(json_array_length(changes)));

    char *s = json_stringify(root);
    json_free(root);
    return s;
}

static char *tool_git_diff(const char *name, const char *args_json, char **error) {
    (void)name; (void)error;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *path_val = json_object_get(args, "path");
    const char *path = path_val && json_is_string(path_val)
        ? json_string_value(path_val) : NULL;
    json_value_t *staged_val = json_object_get(args, "staged");
    bool staged = staged_val && json_is_bool(staged_val) && json_bool_value(staged_val);
    json_value_t *stat_val = json_object_get(args, "stat");
    bool stat = stat_val && json_is_bool(stat_val) && json_bool_value(stat_val);

    char *qpath = path ? shell_quote(path) : NULL;
    size_t cap = 4096 + (path ? strlen(path) * 4 : 0) + 64;
    char *cmd = malloc(cap);
    if (!cmd) {
        free(qpath);
        if (error) *error = strdup("Out of memory");
        json_free(args);
        return NULL;
    }
    int o = snprintf(cmd, cap, "git diff ");
    if (staged) o += snprintf(cmd + o, cap - o, "--cached ");
    if (stat)   o += snprintf(cmd + o, cap - o, "--stat ");
    if (qpath) {
        o += snprintf(cmd + o, cap - o, "-- %s", qpath);
        free(qpath);
    }
    snprintf(cmd + o, cap - o, " 2>&1");

    int ec = 0;
    char *diff = git_run(cmd, &ec);
    free(cmd);

    json_value_t *root = json_object();
    json_object_set(root, "success", json_bool(ec == 0));
    json_object_set(root, "diff", json_string(diff && diff[0] ? diff : ""));
    json_object_set(root, "exit_code", json_number(ec));
    free(diff);

    char *s = json_stringify(root);
    json_free(root);
    json_free(args);
    return s;
}

static char *tool_git_log(const char *name, const char *args_json, char **error) {
    (void)name; (void)error;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }

    json_value_t *n_val = json_object_get(args, "n");
    int n = n_val && json_is_number(n_val) ? (int)json_number_value(n_val) : 10;
    if (n < 1) n = 1;
    if (n > 100) n = 100;
    json_value_t *path_val = json_object_get(args, "path");
    const char *path = path_val && json_is_string(path_val)
        ? json_string_value(path_val) : NULL;

    char *qpath = path ? shell_quote(path) : NULL;
    size_t cap = 4096 + (path ? strlen(path) * 4 : 0) + 64;
    char *cmd = malloc(cap);
    if (!cmd) {
        free(qpath);
        if (error) *error = strdup("Out of memory");
        json_free(args);
        return NULL;
    }
    int o = snprintf(cmd, cap, "git log -n %d %s", n,
                     "--pretty=format:%h%x09%s");
    if (qpath) {
        o += snprintf(cmd + o, cap - o, " -- %s", qpath);
        free(qpath);
    }
    snprintf(cmd + o, cap - o, " 2>&1");

    int ec = 0;
    char *out = git_run(cmd, &ec);
    free(cmd);

    json_value_t *root = json_object();
    if (ec != 0 || !out) {
        json_object_set(root, "success", json_bool(false));
        json_object_set(root, "error",
            json_string(out && out[0] ? out : "not a git repository"));
        free(out);
        char *s = json_stringify(root);
        json_free(root);
        json_free(args);
        return s;
    }

    json_value_t *commits = json_array();
    int count = 0;
    char *saveptr;
    char *line = strtok_r(out, "\n", &saveptr);
    while (line && count < n) {
        char *tab = strchr(line, '\t');
        json_value_t *c = json_object();
        if (tab) {
            *tab = '\0';
            json_object_set(c, "hash", json_string(line));
            json_object_set(c, "subject", json_string(tab + 1));
            *tab = '\t';
        } else {
            json_object_set(c, "hash", json_string(line));
            json_object_set(c, "subject", json_string(""));
        }
        json_array_append(commits, c);
        count++;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(out);

    json_object_set(root, "success", json_bool(true));
    json_object_set(root, "commits", commits);
    json_object_set(root, "count", json_number(count));

    char *s = json_stringify(root);
    json_free(root);
    json_free(args);
    return s;
}

/* ── send_message (mailbox) ───────────────────────────────────── */

static char *tool_send_message(const char *name, const char *args_json, char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) { if (error) *error = strdup("Invalid JSON arguments"); return NULL; }

    json_value_t *recip = json_object_get(args, "recipient");
    json_value_t *subj  = json_object_get(args, "subject");
    json_value_t *body  = json_object_get(args, "body");
    json_value_t *send  = json_object_get(args, "sender");

    const char *recipient = recip ? json_string_value(recip) : "all";
    const char *subject   = subj  ? json_string_value(subj)  : "";
    const char *b         = body  ? json_string_value(body)  : "";
    const char *sender    = send  ? json_string_value(send)  : "agent";

    if (!b[0]) {
        if (error) *error = strdup("Missing 'body' argument");
        json_free(args); return NULL;
    }

    char *msg_id = mailbox_send(sender, recipient, subject, b);
    json_free(args);

    if (!msg_id) {
        if (error) *error = strdup("Failed to send message");
        return NULL;
    }

    json_value_t *out = json_object();
    json_object_set(out, "message_id", json_string(msg_id));
    json_object_set(out, "sent", json_bool(true));

    char *out_str = json_stringify(out);
    json_free(out);
    free(msg_id);
    return out_str;
}

/* ── check_mailbox ─────────────────────────────────────────────── */

static char *tool_check_mailbox(const char *name, const char *args_json, char **error) {
    (void)name; (void)error;
    json_value_t *args = json_parse(args_json);
    const char *recipient = "agent";

    if (args) {
        json_value_t *r = json_object_get(args, "recipient");
        if (r && json_is_string(r)) recipient = json_string_value(r);
        json_free(args);
    }

    json_value_t *out = json_object();
    json_value_t *msgs_arr = json_array();

    mailbox_msg_t *msgs[64];
    int n = mailbox_check(recipient, msgs, 64);

    for (int i = 0; i < n; i++) {
        json_value_t *m = json_object();
        json_object_set(m, "id", json_string(msgs[i]->id));
        json_object_set(m, "sender", json_string(msgs[i]->sender ? msgs[i]->sender : ""));
        json_object_set(m, "subject", json_string(msgs[i]->subject ? msgs[i]->subject : ""));
        json_object_set(m, "body", json_string(msgs[i]->body ? msgs[i]->body : ""));
        json_array_append(msgs_arr, m);
        mailbox_msg_free(msgs[i]);
    }

    json_object_set(out, "messages", msgs_arr);
    json_object_set(out, "count", json_number(n));

    char *out_str = json_stringify(out);
    json_free(out);
    return out_str;
}

/* ── clear_mailbox ──────────────────────────────────────────────── */

static char *tool_clear_mailbox(const char *name, const char *args_json, char **error) {
    (void)name; (void)error;
    json_value_t *args = json_parse(args_json);
    const char *recipient = "agent";
    if (args) {
        json_value_t *r = json_object_get(args, "recipient");
        if (r && json_is_string(r)) recipient = json_string_value(r);
        json_free(args);
    }

    int n = mailbox_clear(recipient);

    json_value_t *out = json_object();
    json_object_set(out, "cleared", json_number(n));
    char *out_str = json_stringify(out);
    json_free(out);
    return out_str;
}

/* ── Registration ───────────────────────────────────────────────── */

void builtin_tools_register(void) {
    /* read_file */
    {
        tool_t *t = tool_create("read_file",
            "Read the contents of a file. Use this to inspect files in the project.",
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to read\"}},"
            "\"required\":[\"path\"]}",
            tool_read_file);
        tool_registry_add(t);
    }

    /* write_file */
    {
        tool_t *t = tool_create("write_file",
            "Write content to a file. Creates or overwrites the file.",
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to write\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
            tool_write_file);
        tool_registry_add(t);
    }

    /* bash */
    {
        tool_t *t = tool_create("bash",
            "Execute a bash command and return its output.",
            "{\"type\":\"object\",\"properties\":{"
            "\"command\":{\"type\":\"string\",\"description\":\"The command to execute\"}},"
            "\"required\":[\"command\"]}",
            tool_bash);
        tool_registry_add(t);
    }

    /* think */
    {
        tool_t *t = tool_create("think",
            "Think about something. Use this to reason through complex problems. "
            "The thought is recorded but not acted upon.",
            "{\"type\":\"object\",\"properties\":{"
            "\"thought\":{\"type\":\"string\",\"description\":\"What to think about\"}},"
            "\"required\":[\"thought\"]}",
            tool_think);
        tool_registry_add(t);
    }

    /* spawn_subagent */
    {
        tool_t *t = tool_create("spawn_subagent",
            "Spawn a subagent to handle a subtask independently. "
            "The subagent runs in a separate process with its own conversation context. "
            "Use this for parallel work or to isolate complex subtasks.",
            "{\"type\":\"object\",\"properties\":{"
            "\"task\":{\"type\":\"string\",\"description\":\"The task for the subagent to complete\"},"
            "\"model\":{\"type\":\"string\",\"description\":\"Model override for subagent\"},"
            "\"provider\":{\"type\":\"string\",\"description\":\"Provider override for subagent\"},"
            "\"system_prompt\":{\"type\":\"string\",\"description\":\"System prompt for the subagent\"}},"
            "\"required\":[\"task\"]}",
            tool_spawn_subagent);
        tool_registry_add(t);
    }

    /* edit */
    {
        tool_t *t = tool_create("edit",
            "Performs exact string replacement in a file. "
            "The old_string must match exactly and be unique in the file. "
            "Use this to make precise edits to source code.",
            "{\"type\":\"object\",\"properties\":{"
            "\"file_path\":{\"type\":\"string\",\"description\":\"Path to the file to edit\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to find and replace\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"file_path\",\"old_string\",\"new_string\"]}",
            tool_edit);
        tool_registry_add(t);
    }

    /* glob */
    {
        tool_t *t = tool_create("glob",
            "Find files matching a glob pattern. "
            "Returns relative file paths. Excludes .git and third_party.",
            "{\"type\":\"object\",\"properties\":{"
            "\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern like '*.c' or 'src/*.h'\"}},"
            "\"required\":[\"pattern\"]}",
            tool_glob);
        tool_registry_add(t);
    }

    /* grep */
    {
        tool_t *t = tool_create("grep",
            "Search for a text pattern in files. Returns matching lines "
            "with file path, line number, and content.",
            "{\"type\":\"object\",\"properties\":{"
            "\"pattern\":{\"type\":\"string\",\"description\":\"Text pattern to search for\"},"
            "\"include\":{\"type\":\"string\",\"description\":\"Optional file pattern filter (e.g. '*.c')\"}},"
            "\"required\":[\"pattern\"]}",
            tool_grep);
        tool_registry_add(t);
    }

    /* web_fetch */
    {
        tool_t *t = tool_create("web_fetch",
            "Fetches content from a specified URL and returns it as text.",
            "{\"type\":\"object\",\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"The URL to fetch content from\"}},"
            "\"required\":[\"url\"]}",
            tool_web_fetch);
        tool_registry_add(t);
    }

    /* web_search */
    {
        tool_t *t = tool_create("web_search",
            "Performs web searches and returns a list of result snippets.",
            "{\"type\":\"object\",\"properties\":{"
            "\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
            tool_web_search);
        tool_registry_add(t);
    }

    /* list_dir */
    {
        tool_t *t = tool_create("list_dir",
            "List the entries of a directory (files and subdirectories) with "
            "their type and size. Use this to understand the project layout.",
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Directory to list (default: .)\"},"
            "\"all\":{\"type\":\"boolean\",\"description\":\"Include hidden files (default: false)\"}}}",
            tool_list_dir);
        tool_registry_add(t);
    }

    /* apply_patch */
    {
        tool_t *t = tool_create("apply_patch",
            "Apply a unified diff patch to files. The patch uses standard "
            "git-style format with ---/+++ file headers and @@ hunks. "
            "Supports adding files (--- /dev/null), deleting files "
            "(+++ /dev/null), and multi-hunk edits. Fails cleanly if any "
            "hunk does not match.",
            "{\"type\":\"object\",\"properties\":{"
            "\"patch\":{\"type\":\"string\",\"description\":\"The unified diff patch text\"},"
            "\"path\":{\"type\":\"string\",\"description\":\"Optional: override the target file path\"}},"
            "\"required\":[\"patch\"]}",
            tool_apply_patch);
        tool_registry_add(t);
    }

    /* git_status */
    {
        tool_t *t = tool_create("git_status",
            "Show the current git repository status: branch and changed files "
            "with their index/worktree status codes (e.g. M, A, D, untracked).",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_git_status);
        tool_registry_add(t);
    }

    /* git_diff */
    {
        tool_t *t = tool_create("git_diff",
            "Show the git diff of unstaged changes (or staged changes with "
            "staged=true). Optionally filter to a single path or show a stat "
            "summary instead of the full diff.",
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Optional: only diff this path\"},"
            "\"staged\":{\"type\":\"boolean\",\"description\":\"Diff staged (--cached) changes\"},"
            "\"stat\":{\"type\":\"boolean\",\"description\":\"Show --stat summary only\"}}}",
            tool_git_diff);
        tool_registry_add(t);
    }

    /* git_log */
    {
        tool_t *t = tool_create("git_log",
            "Show recent git commit history as a list of hashes and subjects. "
            "Optionally limit the number of commits or filter by path.",
            "{\"type\":\"object\",\"properties\":{"
            "\"n\":{\"type\":\"integer\",\"description\":\"Number of commits (default: 10, max 100)\"},"
            "\"path\":{\"type\":\"string\",\"description\":\"Optional: only commits touching this path\"}}}",
            tool_git_log);
        tool_registry_add(t);
    }

    /* send_message */
    {
        tool_t *t = tool_create("send_message",
            "Send a message to the mailbox. Messages can be read later "
            "by the recipient using check_mailbox.",
            "{\"type\":\"object\",\"properties\":{"
            "\"recipient\":{\"type\":\"string\",\"description\":\"Recipient name (default: all)\"},"
            "\"subject\":{\"type\":\"string\",\"description\":\"Message subject\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Message body\"},"
            "\"sender\":{\"type\":\"string\",\"description\":\"Sender name (default: agent)\"}},"
            "\"required\":[\"body\"]}",
            tool_send_message);
        tool_registry_add(t);
    }

    /* check_mailbox */
    {
        tool_t *t = tool_create("check_mailbox",
            "Check the mailbox for unread messages. Returns list of messages.",
            "{\"type\":\"object\",\"properties\":{"
            "\"recipient\":{\"type\":\"string\",\"description\":\"Filter by recipient (default: agent)\"}}}",
            tool_check_mailbox);
        tool_registry_add(t);
    }

    /* clear_mailbox */
    {
        tool_t *t = tool_create("clear_mailbox",
            "Clear (delete) all messages for a recipient from the mailbox.",
            "{\"type\":\"object\",\"properties\":{"
            "\"recipient\":{\"type\":\"string\",\"description\":\"Recipient to clear (default: agent)\"}}}",
            tool_clear_mailbox);
        tool_registry_add(t);
    }
}

/* ── Tool lifecycle helpers ─────────────────────────────────────── */

tool_t *tool_create(const char *name, const char *description,
                    const char *parameters_schema, tool_handler_t handler) {
    tool_t *tool = calloc(1, sizeof(tool_t));
    if (!tool) return NULL;
    tool->name = strdup(name);
    tool->description = strdup(description);
    tool->parameters_schema = strdup(parameters_schema);
    tool->handler = handler;
    return tool;
}

void tool_free(tool_t *tool) {
    if (!tool) return;
    free(tool->name);
    free(tool->description);
    free(tool->parameters_schema);
    free(tool);
}
