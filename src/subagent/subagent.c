/*
 * subagent.c — Subagent spawning and IPC
 *
 * Forks a child process running cgent --subagent.
 * Communication over stdin/stdout pipes with JSON messages.
 *
 * The parent can spawn an async handle, poll for progress, send
 * follow-up instructions mid-task, and stop/abort the child.
 */
#include "subagent.h"
#include "json.h"
#include "tools.h"
#include "protocol.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
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
    json_object_set(root, "model", json_string(cfg->model ? cfg->model : "deepseek-v4-flash"));
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

/* ── Async subagent handle ───────────────────────────────────────── */

struct subagent_handle {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    bool stopped;
    bool got_result;
    bool reaped;
    bool timed_out;
    int exit_code;
    int64_t start_ms;
    int timeout_seconds;
    char *output;               /* first-turn result content */
    char *error;
    subagent_event_fn on_event;
    void *event_ctx;
};

subagent_handle_t *subagent_spawn(subagent_config_t *config, char **err) {
    if (!config || !config->task) {
        if (err) *err = strdup("No task provided");
        return NULL;
    }

    int child_stdin[2];   /* Parent writes → child reads */
    int child_stdout[2];  /* Child writes → parent reads */
    if (pipe(child_stdin) != 0 || pipe(child_stdout) != 0) {
        if (err) *err = strdup("pipe failed");
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = strdup("fork failed");
        close(child_stdin[0]); close(child_stdin[1]);
        close(child_stdout[0]); close(child_stdout[1]);
        return NULL;
    }

    if (pid == 0) {
        /* ── Child process ──────────────────────────────────────── */
        dup2(child_stdin[0], STDIN_FILENO);
        dup2(child_stdout[1], STDOUT_FILENO);
        close(child_stdin[0]); close(child_stdin[1]);
        close(child_stdout[0]); close(child_stdout[1]);

        const char *binary = config->binary_path;
        if (!binary || !binary[0]) binary = "/proc/self/exe";

        execl(binary, "cgent", "--subagent", (char *)NULL);
        extern char **environ;
        execve(binary, (char *[]){"cgent", "--subagent", NULL}, environ);

        fprintf(stderr, "subagent: exec(%s) failed: %s\n",
                binary, strerror(errno));
        _exit(1);
    }

    /* ── Parent process ─────────────────────────────────────────── */
    close(child_stdin[0]);
    close(child_stdout[1]);

    subagent_handle_t *h = calloc(1, sizeof(subagent_handle_t));
    if (!h) {
        if (err) *err = strdup("out of memory");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(child_stdin[1]);
        close(child_stdout[0]);
        return NULL;
    }
    h->pid = pid;
    h->stdin_fd = child_stdin[1];
    h->stdout_fd = child_stdout[0];
    h->start_ms = os_time_ms();
    h->timeout_seconds = config->timeout_seconds > 0
                         ? config->timeout_seconds : 120;
    h->on_event = config->on_event;
    h->event_ctx = config->event_ctx;
    h->exit_code = -1;

    char *task_json = build_task_json(config);
    if (!ipc_write_msg(h->stdin_fd, task_json)) {
        free(task_json);
        if (err) *err = strdup("failed to send task");
        subagent_abort(h);
        subagent_handle_free(h);
        return NULL;
    }
    free(task_json);
    return h;
}

/* Handle one message from the child. Returns true to keep reading. */
static bool handle_message(subagent_handle_t *h, const char *msg) {
    json_value_t *root = json_parse(msg);
    if (!root) return true;

    json_value_t *type = json_object_get(root, "type");
    const char *typestr = type && json_is_string(type)
                          ? json_string_value(type) : "";

    if (strcmp(typestr, "tool_call") == 0) {
        json_value_t *id_val = json_object_get(root, "id");
        json_value_t *name_val = json_object_get(root, "name");
        json_value_t *args_val = json_object_get(root, "arguments");
        const char *tc_id = id_val ? json_string_value(id_val) : "";
        const char *tc_name = name_val ? json_string_value(name_val) : "";
        const char *tc_args = args_val ? json_string_value(args_val) : "{}";

        char *error = NULL;
        char *result = tool_execute(tc_name, tc_args, 30000, &error);

        json_value_t *resp = json_object();
        json_object_set(resp, "type", json_string("tool_result"));
        json_object_set(resp, "id", json_string(tc_id));
        json_object_set(resp, "result", json_string(result ? result : ""));
        if (error) json_object_set(resp, "error", json_string(error));
        char *resp_json = json_stringify(resp);
        ipc_write_msg(h->stdin_fd, resp_json);
        free(resp_json);
        json_free(resp);
        free(result);
        free(error);
    } else if (strcmp(typestr, "result") == 0) {
        json_value_t *content = json_object_get(root, "content");
        json_value_t *err = json_object_get(root, "error");
        if (content && json_is_string(content) && json_string_value(content)[0]) {
            free(h->output);
            h->output = strdup(json_string_value(content));
        }
        if (err && json_is_string(err) && json_string_value(err)[0]) {
            free(h->error);
            h->error = strdup(json_string_value(err));
        }
        h->got_result = true;
        if (h->on_event && h->output) {
            h->on_event(SUBAGENT_EVENT_UPDATE, h->output, h->event_ctx);
        }
    } else if (strcmp(typestr, "update") == 0) {
        json_value_t *content = json_object_get(root, "content");
        if (h->on_event && content && json_is_string(content)) {
            h->on_event(SUBAGENT_EVENT_UPDATE, json_string_value(content),
                        h->event_ctx);
        }
    } else if (strcmp(typestr, "log") == 0) {
        json_value_t *log_msg = json_object_get(root, "message");
        const char *text = log_msg && json_is_string(log_msg)
                           ? json_string_value(log_msg) : "";
        if (h->on_event) {
            h->on_event(SUBAGENT_EVENT_LOG, text, h->event_ctx);
        } else if (text[0]) {
            fprintf(stderr, "[subagent] %s\n", text);
        }
    }

    json_free(root);
    return true;
}

int subagent_poll(subagent_handle_t *h, int timeout_ms) {
    if (!h) return -1;
    int processed = 0;
    int64_t start = os_time_ms();

    while (processed < 100) {
        int remaining = timeout_ms - (int)(os_time_ms() - start);
        if (remaining < 0) remaining = 0;

        struct pollfd pfd = { .fd = h->stdout_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return processed;
        }
        if (pr == 0) break; /* timeout */

        char *msg = ipc_read_msg(h->stdout_fd);
        if (!msg) return -1; /* child closed stdout */
        handle_message(h, msg);
        free(msg);
        processed++;
        timeout_ms = 0; /* drain any immediately-available messages */
    }
    return processed;
}

int subagent_followup(subagent_handle_t *h, const char *content) {
    if (!h || !content) return -1;
    json_value_t *root = json_object();
    json_object_set(root, "type", json_string("followup"));
    json_object_set(root, "content", json_string(content));
    char *json = json_stringify(root);
    json_free(root);
    bool ok = json && ipc_write_msg(h->stdin_fd, json);
    free(json);
    return ok ? 0 : -1;
}

int subagent_stop(subagent_handle_t *h) {
    if (!h || h->stopped) return -1;
    h->stopped = true;
    json_value_t *root = json_object();
    json_object_set(root, "type", json_string("stop"));
    char *json = json_stringify(root);
    json_free(root);
    bool ok = json && ipc_write_msg(h->stdin_fd, json);
    free(json);
    return ok ? 0 : -1;
}

subagent_result_t *subagent_wait(subagent_handle_t *h, int timeout_seconds) {
    if (!h) return NULL;
    if (!h->stopped) subagent_stop(h);
    if (h->stdin_fd >= 0) {
        close(h->stdin_fd);
        h->stdin_fd = -1;
    }

    int timeout = timeout_seconds > 0 ? timeout_seconds : h->timeout_seconds;
    int64_t deadline = os_time_ms() + (int64_t)timeout * 1000;

    while (1) {
        int64_t now = os_time_ms();
        if (now >= deadline) { h->timed_out = true; break; }
        int remaining = (int)(deadline - now);
        int pr = subagent_poll(h, remaining > 500 ? 500 : remaining);
        if (pr < 0) break; /* EOF */

        int status;
        pid_t wpid = waitpid(h->pid, &status, WNOHANG);
        if (wpid == h->pid) {
            h->reaped = true;
            h->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
    }

    if (!h->reaped) {
        if (h->timed_out) kill(h->pid, SIGKILL);
        int status;
        waitpid(h->pid, &status, 0);
        h->reaped = true;
        h->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    subagent_result_t *result = calloc(1, sizeof(subagent_result_t));
    result->exit_code = h->exit_code;
    result->output = h->output ? strdup(h->output) : NULL;
    result->error = h->error ? strdup(h->error) : NULL;
    result->timed_out = h->timed_out;
    result->wall_time_seconds = (os_time_ms() - h->start_ms) / 1000.0;
    return result;
}

void subagent_abort(subagent_handle_t *h) {
    if (!h) return;
    if (!h->reaped && h->pid > 0) {
        kill(h->pid, SIGKILL);
        waitpid(h->pid, NULL, 0);
        h->reaped = true;
    }
}

void subagent_handle_free(subagent_handle_t *h) {
    if (!h) return;
    if (h->stdin_fd >= 0) close(h->stdin_fd);
    if (h->stdout_fd >= 0) close(h->stdout_fd);
    free(h->output);
    free(h->error);
    free(h);
}

/* ── Synchronous wrapper ─────────────────────────────────────────── */

subagent_result_t *subagent_run(subagent_config_t *config) {
    char *err = NULL;
    subagent_handle_t *h = subagent_spawn(config, &err);
    if (!h) {
        fprintf(stderr, "subagent: spawn failed: %s\n",
                err ? err : "unknown error");
        free(err);
        return NULL;
    }

    /* Wait for the first-turn result (the child stays alive for
     * follow-ups until we stop it). */
    int64_t deadline = os_time_ms() + (int64_t)h->timeout_seconds * 1000;
    while (!h->got_result) {
        if (os_time_ms() >= deadline) break;
        int pr = subagent_poll(h, 100);
        if (pr < 0) break;
    }

    subagent_result_t *result = subagent_wait(h, h->timeout_seconds);
    subagent_handle_free(h);
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
    const char *model = model_val ? json_string_value(model_val) : "deepseek-v4-flash";
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

    char *system_str = (system_val && json_is_string(system_val))
                       ? strdup(json_string_value(system_val)) : NULL;
    char *base_url_str = base_url_val ? strdup(json_string_value(base_url_val)) : NULL;

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
        json_free(root);
        free(task_json);
        return 1;
    }
    if (system_str) {
        agent_set_system_prompt(agent, system_str);
        free(system_str);
    }

    builtin_tools_register();
    for (int i = 0; i < registry_count(); i++)
        agent_add_tool(agent, registry_get(i));

    json_free(root);
    free(task_json);

    /* First turn */
    message_t *resp = agent_chat(agent, task_str);
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

    /* Follow-up turns: keep reading stdin until stop or EOF */
    int turn = 1;
    while (1) {
        char *msg = ipc_read_msg(STDIN_FILENO);
        if (!msg) break; /* parent closed stdin */
        json_value_t *fm = json_parse(msg);
        free(msg);
        if (!fm) continue;

        json_value_t *ftype = json_object_get(fm, "type");
        const char *ft = ftype && json_is_string(ftype)
                         ? json_string_value(ftype) : "";
        if (strcmp(ft, "followup") == 0) {
            json_value_t *content_val = json_object_get(fm, "content");
            const char *content = content_val && json_is_string(content_val)
                                  ? json_string_value(content_val) : "";
            if (content[0]) {
                message_t *upd_resp = agent_chat(agent, content);
                json_value_t *upd = json_object();
                json_object_set(upd, "type", json_string("update"));
                json_object_set(upd, "turn", json_number(++turn));
                if (upd_resp && upd_resp->content) {
                    json_object_set(upd, "content", json_string(upd_resp->content));
                } else {
                    json_object_set(upd, "content", json_string(""));
                }
                char *upd_json = json_stringify(upd);
                ipc_write_msg(STDOUT_FILENO, upd_json);
                free(upd_json);
                json_free(upd);
                message_free(upd_resp);
            }
        } else if (strcmp(ft, "stop") == 0) {
            json_free(fm);
            break;
        }
        json_free(fm);
    }

    agent_free(agent);
    return 0;
}
