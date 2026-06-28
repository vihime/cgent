/*
 * builtin_tools.c — Built-in tools (read_file, write_file, bash, think, subagent)
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
