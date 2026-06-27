/*
 * subagent.c — Subagent spawning and IPC
 *
 * Forks a child process running cgent --subagent.
 * Communication over stdin/stdout pipes with JSON messages.
 */
#include "subagent.h"
#include "json.h"
#include "tools.h"
#include "protocol.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

/* ── IPC helpers ─────────────────────────────────────────────────── */

/* Write a JSON message to a file descriptor, prefixed with length */
static bool ipc_write_msg(int fd, const char *json) {
    if (!json) return false;
    size_t len = strlen(json);
    /* Write: 8-byte hex length + newline + JSON */
    char header[32];
    snprintf(header, sizeof(header), "%08zx\n", len);
    if (write(fd, header, 9) != 9) return false;
    if (write(fd, json, len) != (ssize_t)len) return false;
    if (write(fd, "\n", 1) != 1) return false;
    return true;
}

/* Read a JSON message from a file descriptor.
 * Returns malloc'd string, or NULL on EOF/error. */
static char *ipc_read_msg(int fd) {
    /* Read 8-char hex length */
    char header[9] = {0};
    size_t pos = 0;
    while (pos < 8) {
        ssize_t n = read(fd, header + pos, 8 - pos);
        if (n <= 0) return NULL;
        pos += n;
    }
    /* Read the newline separator */
    char nl;
    if (read(fd, &nl, 1) != 1 || nl != '\n') return NULL;

    long len = strtol(header, NULL, 16);
    if (len <= 0 || len > (16 * 1024 * 1024)) return NULL; /* Max 16MB */

    char *buf = malloc(len + 1);
    if (!buf) return NULL;

    pos = 0;
    while (pos < (size_t)len) {
        ssize_t n = read(fd, buf + pos, len - pos);
        if (n <= 0) { free(buf); return NULL; }
        pos += n;
    }
    buf[len] = '\0';

    /* Read trailing newline */
    if (read(fd, &nl, 1) < 0) { /* ignore */ }
    return buf;
}

/* ── Build task JSON ─────────────────────────────────────────────── */

static char *build_task_json(subagent_config_t *cfg) {
    json_value_t *root = json_object();
    json_object_set(root, "type", json_string("task"));
    json_object_set(root, "provider", json_string(cfg->provider ? cfg->provider : "deepseek"));
    json_object_set(root, "model", json_string(cfg->model ? cfg->model : "deepseek-chat"));
    json_object_set(root, "api_key", json_string(cfg->api_key ? cfg->api_key : ""));
    if (cfg->base_url)
        json_object_set(root, "base_url", json_string(cfg->base_url));
    if (cfg->system_prompt)
        json_object_set(root, "system_prompt", json_string(cfg->system_prompt));
    json_object_set(root, "task", json_string(cfg->task ? cfg->task : ""));
    json_object_set(root, "temperature", json_number(cfg->temperature));
    json_object_set(root, "max_tokens", json_number(cfg->max_tokens));

    char *result = json_stringify(root);
    json_free(root);
    return result;
}

/* ── Subagent run (parent side) ──────────────────────────────────── */

subagent_result_t *subagent_run(subagent_config_t *config) {
    if (!config || !config->task) return NULL;

    int child_stdin[2];   /* Parent writes → child reads */
    int child_stdout[2];  /* Child writes → parent reads */

    if (pipe(child_stdin) != 0 || pipe(child_stdout) != 0) {
        perror("pipe");
        return NULL;
    }

    int64_t start_ms = os_time_ms();
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        close(child_stdin[0]); close(child_stdin[1]);
        close(child_stdout[0]); close(child_stdout[1]);
        return NULL;
    }

    if (pid == 0) {
        /* ── Child process ──────────────────────────────────────── */
        /* Redirect stdin/stdout to pipes */
        dup2(child_stdin[0], STDIN_FILENO);
        dup2(child_stdout[1], STDOUT_FILENO);

        /* Close all pipe fds */
        close(child_stdin[0]); close(child_stdin[1]);
        close(child_stdout[0]); close(child_stdout[1]);

        /* Resolve cgent binary path */
        const char *binary = config->binary_path;
        if (!binary || !binary[0]) {
            binary = "/proc/self/exe";  /* Works when cgent spawns itself */
        }

        /* Exec cgent with --subagent flag */
        execl(binary, "cgent", "--subagent", (char *)NULL);

        /* If execl fails, try execve */
        extern char **environ;
        execve(binary, (char *[]){"cgent", "--subagent", NULL}, environ);

        /* Failed — write error to stdout so parent can read it */
        fprintf(stderr, "subagent: exec(%s) failed: %s\n", binary, strerror(errno));
        _exit(1);
    }

    /* ── Parent process ──────────────────────────────────────────── */
    close(child_stdin[0]);   /* Parent doesn't read from child stdin */
    close(child_stdout[1]);  /* Parent doesn't write to child stdout */

    /* Send task JSON to child */
    char *task_json = build_task_json(config);
    if (!ipc_write_msg(child_stdin[1], task_json)) {
        free(task_json);
        close(child_stdin[1]);
        close(child_stdout[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    free(task_json);

    /* Read responses from child */
    char *final_output = NULL;
    char *final_error = NULL;
    bool timed_out = false;

    while (1) {
        char *msg = ipc_read_msg(child_stdout[0]);
        if (!msg) break; /* EOF or error */

        json_value_t *root = json_parse(msg);
        if (!root) { free(msg); continue; }

        json_value_t *type = json_object_get(root, "type");
        const char *typestr = type ? json_string_value(type) : "";

        if (strcmp(typestr, "tool_call") == 0) {
            /* Child needs a tool executed */
            json_value_t *id_val = json_object_get(root, "id");
            json_value_t *name_val = json_object_get(root, "name");
            json_value_t *args_val = json_object_get(root, "arguments");

            const char *tc_id = id_val ? json_string_value(id_val) : "";
            const char *tc_name = name_val ? json_string_value(name_val) : "";
            const char *tc_args = args_val ? json_string_value(args_val) : "{}";

            /* Execute tool in parent */
            char *error = NULL;
            char *result = tool_execute(tc_name, tc_args, 30000, &error);

            /* Send result back */
            json_value_t *resp = json_object();
            json_object_set(resp, "type", json_string("tool_result"));
            json_object_set(resp, "id", json_string(tc_id));
            json_object_set(resp, "result", json_string(result ? result : ""));
            if (error) json_object_set(resp, "error", json_string(error));

            char *resp_json = json_stringify(resp);
            ipc_write_msg(child_stdin[1], resp_json);

            free(resp_json);
            json_free(resp);
            free(result);
            free(error);
        } else if (strcmp(typestr, "result") == 0) {
            /* Child finished */
            json_value_t *content = json_object_get(root, "content");
            json_value_t *err = json_object_get(root, "error");

            if (content && json_is_string(content) && json_string_value(content)[0]) {
                final_output = strdup(json_string_value(content));
            }
            if (err && json_is_string(err) && json_string_value(err)[0]) {
                final_error = strdup(json_string_value(err));
            }
            json_free(root);
            free(msg);
            break; /* Done */
        } else if (strcmp(typestr, "log") == 0) {
            /* Log message from child — print to stderr */
            json_value_t *log_msg = json_object_get(root, "message");
            if (log_msg && json_is_string(log_msg)) {
                fprintf(stderr, "[subagent] %s\n", json_string_value(log_msg));
            }
        }

        json_free(root);
        free(msg);
    }

    /* Close stdin to signal child we're done */
    close(child_stdin[1]);
    close(child_stdout[0]);

    /* Wait for child to exit (with timeout) */
    int timeout = config->timeout_seconds > 0 ? config->timeout_seconds : 120;
    int status;
    pid_t wpid;
    int waited = 0;

    while (waited < timeout * 10) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) break;
        if (wpid < 0 && errno != EINTR) break;
        usleep(100000); /* 100ms */
        waited++;
    }

    if (wpid != pid) {
        /* Timeout — kill child */
        timed_out = true;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    subagent_result_t *result = calloc(1, sizeof(subagent_result_t));
    result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result->output = final_output;
    result->error = final_error;
    result->timed_out = timed_out;
    result->wall_time_seconds = (os_time_ms() - start_ms) / 1000.0;

    return result;
}

void subagent_result_free(subagent_result_t *result) {
    if (!result) return;
    free(result->output);
    free(result->error);
    free(result);
}

/* ── Subagent main (child side) ──────────────────────────────────── */

int subagent_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Disable stdout buffering for IPC */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Read task from stdin */
    char *task_json = ipc_read_msg(STDIN_FILENO);
    if (!task_json) {
        fprintf(stderr, "subagent: failed to read task from stdin\n");
        /* Send error result so parent doesn't hang */
        fprintf(stdout, "00000009\n");
        fprintf(stdout, "{\"type\":\"result\",\"error\":\"read failed\"}\n");
        fflush(stdout);
        return 1;
    }

    json_value_t *root = json_parse(task_json);
    if (!root) {
        fprintf(stderr, "subagent: failed to parse task JSON\n");
        free(task_json);
        return 1;
    }

    /* Extract task parameters */
    json_value_t *provider_val = json_object_get(root, "provider");
    json_value_t *model_val = json_object_get(root, "model");
    json_value_t *api_key_val = json_object_get(root, "api_key");
    json_value_t *base_url_val = json_object_get(root, "base_url");
    json_value_t *system_val = json_object_get(root, "system_prompt");
    json_value_t *task_val = json_object_get(root, "task");
    json_value_t *temp_val = json_object_get(root, "temperature");
    json_value_t *max_tok_val = json_object_get(root, "max_tokens");

    const char *provider_name = provider_val ? json_string_value(provider_val) : "deepseek";
    const char *model = model_val ? json_string_value(model_val) : "deepseek-chat";
    const char *api_key = api_key_val ? json_string_value(api_key_val) : "";
    const char *task_str = task_val ? json_string_value(task_val) : "Complete the task.";

    provider_init();
    api_provider_t *api = provider_get_by_name(provider_name);
    if (!api) {
        json_value_t *resp = json_object();
        json_object_set(resp, "type", json_string("result"));
        json_object_set(resp, "error", json_string("Unknown provider"));
        char *resp_json = json_stringify(resp);
        ipc_write_msg(STDOUT_FILENO, resp_json);
        free(resp_json);
        json_free(resp);
        json_free(root);
        free(task_json);
        return 1;
    }

    /* Extract strings before freeing JSON (json_string_value points into cJSON memory) */
    char *system_str = (system_val && json_is_string(system_val))
                       ? strdup(json_string_value(system_val)) : NULL;
    char *base_url_str = base_url_val ? strdup(json_string_value(base_url_val)) : NULL;
    char *task_copy = strdup(task_str);

    /* Build provider config */
    provider_config_t pcfg = {
        .api_key     = strdup(api_key),
        .base_url    = base_url_str ? base_url_str : strdup(api->default_base_url),
        .model       = strdup(model),
        .temperature = temp_val ? json_number_value(temp_val) : 0.7,
        .max_tokens  = max_tok_val ? (int)json_number_value(max_tok_val) : 1024,
        .stream      = false,  /* Subagents use non-streaming */
    };

    agent_t *agent = agent_create(&pcfg, api);
    if (!agent) {
        free(system_str);
        free(task_copy);
        json_free(root);
        free(task_json);
        return 1;
    }

    if (system_str) {
        agent_set_system_prompt(agent, system_str);
        free(system_str);
    }

    /* Register built-in tools */
    builtin_tools_register();
    for (int i = 0; i < registry_count(); i++)
        agent_add_tool(agent, registry_get(i));

    json_free(root);
    free(task_json);

    /* Run the agent chat loop */
    message_t *resp = agent_chat(agent, task_copy);
    free(task_copy);

    /* Send result back to parent */
    json_value_t *result = json_object();
    json_object_set(result, "type", json_string("result"));
    if (resp && resp->content) {
        json_object_set(result, "content", json_string(resp->content));
    } else {
        json_object_set(result, "content", json_string(""));
        json_object_set(result, "error", json_string("No response from agent"));
    }

    char *result_json = json_stringify(result);
    ipc_write_msg(STDOUT_FILENO, result_json);

    free(result_json);
    json_free(result);
    message_free(resp);
    agent_free(agent);

    return 0;
}
