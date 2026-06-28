/*
 * builtin_tools.c — Built-in tools (read_file, write_file, edit, bash, think, glob, grep, subagent)
 */
#include "tools.h"
#include "json.h"
#include "platform.h"
#include "subagent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    char *content = malloc(size + 1);
    if (!content) {
        fclose(fp);
        json_free(args);
        if (error) *error = strdup("Memory allocation failed");
        return NULL;
    }

    size_t nread = fread(content, 1, size, fp);
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
    char *output = os_exec_capture(command, &exit_code);
    json_free(args);

    if (!output) {
        if (error) *error = strdup("Command execution failed");
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
        .model        = (char *)(model_val ? json_string_value(model_val) : "deepseek-chat"),
        .api_key      = NULL,
        .system_prompt = (char *)(prompt_val ? json_string_value(prompt_val) : NULL),
        .task         = (char *)json_string_value(task_val),
        .temperature  = 0.0,
        .max_tokens   = 2048,
        .timeout_seconds = 120,
    };

    /* Resolve API key from environment (provider-specific) */
    if (!cfg.api_key) cfg.api_key = os_getenv("DEEPSEEK_API_KEY");
    if (!cfg.api_key) cfg.api_key = os_getenv("OPENAI_API_KEY");
    if (!cfg.api_key) cfg.api_key = os_getenv("ANTHROPIC_API_KEY");

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
    char cmd[2048];
    /* Escape quotes in pattern for shell safety */
    snprintf(cmd, sizeof(cmd),
             "find . -path './.git' -prune -o -path './third_party' -prune "
             "-o -name '%s' -print 2>/dev/null | head -200", pattern);

    int exit_code;
    char *output = os_exec_capture(cmd, &exit_code);

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

    /* Build grep command */
    char cmd[4096];
    if (include) {
        /* Escape single quotes in pattern */
        snprintf(cmd, sizeof(cmd),
                 "grep -rn --include='%s' '%s' . "
                 "--exclude-dir=.git --exclude-dir=third_party "
                 "--exclude-dir=.cgent 2>/dev/null | head -200",
                 include, pattern);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "grep -rn '%s' . "
                 "--exclude-dir=.git --exclude-dir=third_party "
                 "--exclude-dir=.cgent 2>/dev/null | head -200",
                 pattern);
    }

    int exit_code;
    char *output = os_exec_capture(cmd, &exit_code);

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

    /* Use curl to fetch the URL */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 30 --connect-timeout 10 "
             "-H 'User-Agent: cgent/0.1' '%s' 2>/dev/null | head -c 65536",
             url);

    int exit_code;
    char *output = os_exec_capture(cmd, &exit_code);

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

    /* Use curl to fetch search results from DuckDuckGo HTML */
    char cmd[8192];
    /* URL-encode the query (basic: replace spaces and special chars) */
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 30 --connect-timeout 10 "
             "-H 'User-Agent: cgent/0.1' "
             "'https://html.duckduckgo.com/html/?q=%s' 2>/dev/null | "
             "grep -oP 'class=\"result__snippet\">\\K[^<]+' | head -20",
             query);

    int exit_code;
    char *output = os_exec_capture(cmd, &exit_code);

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
