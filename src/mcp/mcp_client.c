/*
 * mcp_client.c — Minimal MCP stdio client and runtime session
 *
 * MCP servers are spawned as child processes and speak JSON-RPC 2.0
 * over stdin/stdout, with one JSON message per line (newline-delimited).
 * The session API performs the initialize handshake, lists tools, and
 * forwards tools/call invocations; mcp_server_test builds on it for
 * the `cgent mcp test` command.
 */
#include "mcp.h"
#include "json.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Child process handle ───────────────────────────────────────── */

typedef struct {
    pid_t pid;
    int in_fd;      /* parent -> child stdin */
    int out_fd;     /* child stdout -> parent */
    int err_fd;     /* child stderr -> parent */
    char *rbuf;     /* buffered stdout reader */
    size_t rlen, rcap;
} mcp_proc_t;

static int read_into_buffer(int fd, char **buf, size_t *len, size_t *cap) {
    if (*len + 4096 >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 16384;
        char *grown = realloc(*buf, new_cap);
        if (!grown) return -1;
        *buf = grown;
        *cap = new_cap;
    }
    ssize_t n = read(fd, *buf + *len, *cap - *len - 1);
    if (n <= 0) return (n == 0) ? 0 : -1;
    *len += (size_t)n;
    (*buf)[*len] = '\0';
    return 1;
}

/* Read one newline-delimited line from the child's stdout with timeout.
 * Returns a malloc'd line (without the newline) or NULL on EOF/timeout. */
static char *proc_readline(mcp_proc_t *p, int timeout_ms) {
    int64_t start = os_time_ms();
    while (1) {
        char *nl = p->rbuf ? memchr(p->rbuf, '\n', p->rlen) : NULL;
        if (nl) {
            size_t llen = (size_t)(nl - p->rbuf);
            char *line = strndup(p->rbuf, llen);
            memmove(p->rbuf, nl + 1, p->rlen - llen - 1);
            p->rlen -= llen + 1;
            if (p->rbuf) p->rbuf[p->rlen] = '\0';
            return line;
        }

        int remaining = timeout_ms - (int)(os_time_ms() - start);
        if (remaining <= 0) return NULL;
        if (remaining > 500) remaining = 500;

        struct pollfd fd = { .fd = p->out_fd, .events = POLLIN };
        int pr = poll(&fd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (pr == 0) continue; /* loop re-checks total timeout */
        if (read_into_buffer(p->out_fd, &p->rbuf, &p->rlen, &p->rcap) <= 0)
            return NULL; /* EOF or read error */
    }
}

static mcp_proc_t *proc_spawn(const mcp_server_t *srv, char **err) {
    int inpipe[2], outpipe[2], errpipe[2];
    if (pipe(inpipe) != 0 || pipe(outpipe) != 0 || pipe(errpipe) != 0) {
        if (err) *err = strdup("Failed to create pipes");
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        if (err) *err = strdup("Failed to fork");
        return NULL;
    }

    if (pid == 0) {
        /* Child */
        setpgid(0, 0);
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);

        if (srv->cwd && srv->cwd[0] && chdir(srv->cwd) != 0)
            _exit(126);
        for (int i = 0; i < srv->env_count; i++)
            setenv(srv->env_keys[i], srv->env_values[i], 1);

        char **argv = calloc(srv->arg_count + 2, sizeof(char *));
        if (!argv) _exit(127);
        argv[0] = srv->command;
        for (int i = 0; i < srv->arg_count; i++)
            argv[i + 1] = srv->args[i];
        execvp(argv[0], argv);
        _exit(127);
    }

    close(inpipe[0]);
    close(outpipe[1]);
    close(errpipe[1]);

    mcp_proc_t *p = calloc(1, sizeof(mcp_proc_t));
    if (!p) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(inpipe[1]); close(outpipe[0]); close(errpipe[0]);
        if (err) *err = strdup("Out of memory");
        return NULL;
    }
    p->pid = pid;
    p->in_fd = inpipe[1];
    p->out_fd = outpipe[0];
    p->err_fd = errpipe[0];
    return p;
}

static int proc_write(mcp_proc_t *p, const char *data) {
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t n = write(p->in_fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Drain the child's stderr (non-blocking) and return the last ~2KB. */
static char *proc_stderr_tail(mcp_proc_t *p) {
    fcntl(p->err_fd, F_SETFL, O_NONBLOCK);
    size_t cap = 8192, len = 0;
    char *acc = malloc(cap);
    if (!acc) return NULL;
    acc[0] = '\0';
    char chunk[4096];
    for (int guard = 0; guard < 64; guard++) {
        ssize_t n = read(p->err_fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        if (len + (size_t)n + 1 >= cap) break;
        memcpy(acc + len, chunk, (size_t)n);
        len += (size_t)n;
        acc[len] = '\0';
    }
    if (len > 2000) {
        memmove(acc, acc + len - 2000, 2000);
        acc[2000] = '\0';
    }
    return acc;
}

static void proc_free(mcp_proc_t *p) {
    if (!p) return;
    kill(-p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    close(p->in_fd);
    close(p->out_fd);
    close(p->err_fd);
    free(p->rbuf);
    free(p);
}

/* ── Session ────────────────────────────────────────────────────── */

struct mcp_session {
    mcp_proc_t *proc;
    int next_id;
    bool alive;
    char *server_name;
    char *server_version;
    char *protocol_version;
};

/* Send a JSON-RPC request and wait for the matching id response.
 * Returns the parsed response object (caller json_free) or NULL. */
static json_value_t *session_request(mcp_session_t *s, int id,
                                     const char *method,
                                     json_value_t *params,
                                     int timeout_ms, char **err) {
    json_value_t *req = json_object();
    json_object_set(req, "jsonrpc", json_string("2.0"));
    json_object_set(req, "id", json_number(id));
    json_object_set(req, "method", json_string(method));
    if (params) json_object_set(req, "params", params);
    char *req_str = json_stringify(req);
    json_free(req);
    if (!req_str) {
        if (err) *err = strdup("Out of memory");
        return NULL;
    }
    /* MCP stdio is newline-delimited: append the line terminator */
    size_t rl = strlen(req_str);
    char *req_line = realloc(req_str, rl + 2);
    if (!req_line) {
        free(req_str);
        if (err) *err = strdup("Out of memory");
        return NULL;
    }
    req_line[rl] = '\n';
    req_line[rl + 1] = '\0';
    if (proc_write(s->proc, req_line) != 0) {
        free(req_line);
        s->alive = false;
        if (err) *err = strdup("Failed to write request to MCP server");
        return NULL;
    }
    free(req_line);

    json_value_t *resp = NULL;
    int64_t start = os_time_ms();
    while (os_time_ms() - start < timeout_ms) {
        char *line = proc_readline(s->proc,
            timeout_ms - (int)(os_time_ms() - start));
        if (!line) break;
        json_value_t *root = json_parse(line);
        free(line);
        if (!root) continue;
        json_value_t *rid = json_object_get(root, "id");
        if (rid && json_is_number(rid) && json_number_value(rid) == (double)id) {
            resp = root;
            break;
        }
        json_free(root);
    }
    if (!resp) {
        s->alive = false;
        if (err) *err = strdup("No response from MCP server (timeout or dead process)");
        return NULL;
    }

    json_value_t *rpc_err = json_object_get(resp, "error");
    if (rpc_err && json_is_object(rpc_err)) {
        json_value_t *m = json_object_get(rpc_err, "message");
        if (err) *err = strdup(m && json_is_string(m)
                               ? json_string_value(m) : "JSON-RPC error");
        json_free(resp);
        return NULL;
    }
    return resp;
}

/* Send a notification (no id, no response expected). */
static void session_notify(mcp_session_t *s, const char *method) {
    json_value_t *note = json_object();
    json_object_set(note, "jsonrpc", json_string("2.0"));
    json_object_set(note, "method", json_string(method));
    char *str = json_stringify(note);
    json_free(note);
    if (str) {
        size_t sl = strlen(str);
        char *line = realloc(str, sl + 2);
        if (line) {
            line[sl] = '\n';
            line[sl + 1] = '\0';
            proc_write(s->proc, line);
            free(line);
        } else {
            free(str);
        }
    }
}

mcp_session_t *mcp_session_start(const mcp_server_t *srv, int timeout_ms,
                                 char **err) {
    if (!srv || !srv->command || !srv->command[0]) {
        if (err) *err = strdup("Server has no command configured");
        return NULL;
    }
    mcp_proc_t *proc = proc_spawn(srv, err);
    if (!proc) return NULL;

    mcp_session_t *s = calloc(1, sizeof(mcp_session_t));
    if (!s) {
        proc_free(proc);
        if (err) *err = strdup("Out of memory");
        return NULL;
    }
    s->proc = proc;
    s->next_id = 1;
    s->alive = true;

    json_value_t *params = json_object();
    json_value_t *caps = json_object();
    json_object_set(params, "capabilities", caps);
    json_value_t *client = json_object();
    json_object_set(client, "name", json_string("cgent"));
    json_object_set(client, "version", json_string("0.1"));
    json_object_set(params, "clientInfo", client);
    json_object_set(params, "protocolVersion", json_string("2024-11-05"));

    char *rpc_err = NULL;
    json_value_t *resp = session_request(s, s->next_id++, "initialize",
                                         params, timeout_ms, &rpc_err);
    if (!resp) {
        char *tail = proc_stderr_tail(proc);
        if (err) {
            size_t n = strlen(rpc_err ? rpc_err : "initialize failed")
                       + (tail ? strlen(tail) : 0) + 64;
            char *msg = malloc(n);
            snprintf(msg, n, "%s%s%s",
                     rpc_err ? rpc_err : "initialize failed",
                     tail && tail[0] ? ": " : "",
                     tail && tail[0] ? tail : "");
            *err = msg;
        }
        free(rpc_err);
        free(tail);
        mcp_session_stop(s);
        return NULL;
    }

    json_value_t *res = json_object_get(resp, "result");
    if (!res || !json_is_object(res)) {
        if (err) *err = strdup("initialize response missing result");
        json_free(resp);
        mcp_session_stop(s);
        return NULL;
    }
    json_value_t *pv = json_object_get(res, "protocolVersion");
    if (pv && json_is_string(pv))
        s->protocol_version = strdup(json_string_value(pv));
    json_value_t *info = json_object_get(res, "serverInfo");
    if (info && json_is_object(info)) {
        json_value_t *n = json_object_get(info, "name");
        json_value_t *v = json_object_get(info, "version");
        if (n && json_is_string(n)) s->server_name = strdup(json_string_value(n));
        if (v && json_is_string(v)) s->server_version = strdup(json_string_value(v));
    }
    json_free(resp);

    session_notify(s, "notifications/initialized");
    return s;
}

int mcp_session_list_tools(mcp_session_t *s, mcp_tool_info_t **tools,
                           int *count, char **err) {
    *tools = NULL;
    *count = 0;
    if (!s || !s->alive) {
        if (err) *err = strdup("MCP session is not running");
        return -1;
    }
    json_value_t *params = json_object();
    char *rpc_err = NULL;
    json_value_t *resp = session_request(s, s->next_id++, "tools/list",
                                         params, 15000, &rpc_err);
    if (!resp) {
        if (err) *err = rpc_err ? rpc_err : strdup("tools/list failed");
        else free(rpc_err);
        return -1;
    }
    json_value_t *res = json_object_get(resp, "result");
    if (!res || !json_is_object(res)) {
        if (err) *err = strdup("tools/list response missing result");
        json_free(resp);
        return -1;
    }
    json_value_t *arr = json_object_get(res, "tools");
    if (arr && json_is_array(arr)) {
        int n = json_array_length(arr);
        for (int i = 0; i < n; i++) {
            json_value_t *t = json_array_get(arr, i);
            if (!json_is_object(t)) continue;
            json_value_t *name = json_object_get(t, "name");
            if (!name || !json_is_string(name) || !json_string_value(name)[0])
                continue;
            mcp_tool_info_t *grown = realloc(*tools,
                (*count + 1) * sizeof(mcp_tool_info_t));
            if (!grown) {
                if (err) *err = strdup("Out of memory");
                json_free(resp);
                return -1;
            }
            *tools = grown;
            mcp_tool_info_t *ti = &(*tools)[*count];
            memset(ti, 0, sizeof(*ti));
            ti->name = strdup(json_string_value(name));
            json_value_t *desc = json_object_get(t, "description");
            ti->description = desc && json_is_string(desc)
                ? strdup(json_string_value(desc)) : strdup("");
            json_value_t *schema = json_object_get(t, "inputSchema");
            if (schema && json_is_object(schema))
                ti->input_schema = json_stringify(schema);
            if (!ti->input_schema)
                ti->input_schema = strdup("{\"type\":\"object\"}");
            (*count)++;
        }
    }
    json_free(resp);
    return 0;
}

#define CALL_CONTENT_CAP (64 * 1024)

char *mcp_session_call_tool(mcp_session_t *s, const char *tool_name,
                            const char *args_json, int timeout_ms,
                            char **err) {
    if (!s || !s->alive) {
        if (err) *err = strdup("MCP session is not running");
        return NULL;
    }
    json_value_t *params = json_object();
    json_object_set(params, "name", json_string(tool_name));
    json_value_t *args = args_json ? json_parse(args_json) : NULL;
    json_object_set(params, "arguments", args ? args : json_object());

    char *rpc_err = NULL;
    json_value_t *resp = session_request(s, s->next_id++, "tools/call",
                                         params, timeout_ms, &rpc_err);
    if (!resp) {
        if (err) *err = rpc_err ? rpc_err : strdup("tools/call failed");
        else free(rpc_err);
        return NULL;
    }

    json_value_t *res = json_object_get(resp, "result");
    if (!res || !json_is_object(res)) {
        if (err) *err = strdup("tools/call response missing result");
        json_free(resp);
        return NULL;
    }

    json_value_t *out = json_object();
    json_value_t *is_err = json_object_get(res, "isError");
    bool is_error = is_err && json_is_bool(is_err) && json_bool_value(is_err);
    json_object_set(out, "is_error", json_bool(is_error));

    /* Concatenate text content; summarize non-text items */
    json_value_t *content = json_object_get(res, "content");
    size_t cap = 8192, len = 0;
    char *text = malloc(cap);
    if (!text) {
        json_free(out);
        json_free(resp);
        if (err) *err = strdup("Out of memory");
        return NULL;
    }
    text[0] = '\0';
    bool truncated = false;
    if (content && json_is_array(content)) {
        int n = json_array_length(content);
        for (int i = 0; i < n && !truncated; i++) {
            json_value_t *item = json_array_get(content, i);
            json_value_t *type = json_object_get(item, "type");
            const char *t = type && json_is_string(type)
                ? json_string_value(type) : "text";
            const char *frag = NULL;
            char tmp[128];
            if (strcmp(t, "text") == 0) {
                json_value_t *tv = json_object_get(item, "text");
                frag = tv && json_is_string(tv) ? json_string_value(tv) : "";
            } else if (strcmp(t, "image") == 0) {
                snprintf(tmp, sizeof(tmp), "[image content (%s)]",
                    (json_object_get(item, "mimeType") &&
                     json_is_string(json_object_get(item, "mimeType")))
                        ? json_string_value(json_object_get(item, "mimeType"))
                        : "unknown type");
                frag = tmp;
            } else {
                snprintf(tmp, sizeof(tmp), "[%s content, not shown]", t);
                frag = tmp;
            }
            size_t fl = strlen(frag);
            if (len + fl + 2 >= CALL_CONTENT_CAP) {
                truncated = true;
                break;
            }
            if (len + fl + 2 >= cap) {
                cap *= 2;
                char *grown = realloc(text, cap);
                if (!grown) break;
                text = grown;
            }
            if (len > 0) text[len++] = '\n';
            memcpy(text + len, frag, fl);
            len += fl;
            text[len] = '\0';
        }
    }
    if (truncated) {
        int olen = snprintf(text + len, 128,
                            "\n... (content truncated at %d bytes)\n",
                            CALL_CONTENT_CAP);
        len += olen > 0 ? (size_t)olen : 0;
        text[len] = '\0';
    }
    json_object_set(out, "content", json_string(text));
    free(text);

    json_value_t *sc = json_object_get(res, "structuredContent");
    if (sc && json_is_object(sc))
        json_object_set(out, "structured", json_dup(sc));

    char *sout = json_stringify(out);
    json_free(out);
    json_free(resp);
    return sout;
}

void mcp_tool_info_free(mcp_tool_info_t *tools, int count) {
    if (!tools) return;
    for (int i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].description);
        free(tools[i].input_schema);
    }
    free(tools);
}

void mcp_session_stop(mcp_session_t *s) {
    if (!s) return;
    s->alive = false;
    proc_free(s->proc);
    free(s->server_name);
    free(s->server_version);
    free(s->protocol_version);
    free(s);
}

/* ── Server test (used by `cgent mcp test`) ─────────────────────── */

static char *test_result(bool success, const char *error_msg,
                         const mcp_server_t *srv) {
    json_value_t *out = json_object();
    json_object_set(out, "success", json_bool(success));
    if (srv && srv->name)
        json_object_set(out, "server", json_string(srv->name));
    if (error_msg)
        json_object_set(out, "error", json_string(error_msg));
    char *s = json_stringify_pretty(out);
    json_free(out);
    return s;
}

char *mcp_server_test(const mcp_server_t *srv, int timeout_ms) {
    if (!srv || !srv->command || !srv->command[0])
        return test_result(false, "Server has no command configured", srv);

    char *err = NULL;
    mcp_session_t *s = mcp_session_start(srv, timeout_ms, &err);
    if (!s) {
        char *r = test_result(false, err ? err : "Failed to start server", srv);
        free(err);
        return r;
    }

    mcp_tool_info_t *tools = NULL;
    int n = 0;
    int rc = mcp_session_list_tools(s, &tools, &n, &err);

    json_value_t *out = json_object();
    json_object_set(out, "success", json_bool(true));
    json_object_set(out, "server",
        json_string(s->server_name ? s->server_name : ""));
    json_object_set(out, "version",
        json_string(s->server_version ? s->server_version : ""));
    json_object_set(out, "protocol_version",
        json_string(s->protocol_version ? s->protocol_version : ""));
    if (rc != 0)
        json_object_set(out, "tools_error", json_bool(true));
    json_value_t *arr = json_array();
    for (int i = 0; i < n; i++)
        json_array_append(arr, json_string(tools[i].name));
    json_object_set(out, "tools_count", json_number(n));
    json_object_set(out, "tools", arr);
    char *tail = proc_stderr_tail(s->proc);
    if (tail && tail[0])
        json_object_set(out, "stderr", json_string(tail));
    free(tail);

    char *sout = json_stringify_pretty(out);
    json_free(out);
    free(err);
    mcp_tool_info_free(tools, n);
    mcp_session_stop(s);
    return sout;
}
