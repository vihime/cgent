/*
 * subagent.h — Subagent spawning and IPC
 *
 * Subagents are child processes that run cgent in --subagent mode.
 * Communication happens over stdin/stdout pipes using JSON messages.
 *
 * IPC Protocol (parent → child, over child's stdin):
 *   {"type":"task","provider":"deepseek","model":"...","api_key":"...",
 *    "system_prompt":"...","messages":[...],"tools":[...]}
 *   {"type":"tool_result","id":"call_xxx","result":"..."}
 *   {"type":"followup","content":"..."}   (extra instruction mid-task)
 *   {"type":"stop"}                       (finish after current turn)
 *
 * IPC Protocol (child → parent, over child's stdout):
 *   {"type":"tool_call","id":"call_xxx","name":"...","arguments":"..."}
 *   {"type":"result","content":"...","error":null}
 *   {"type":"update","turn":N,"content":"..."}   (follow-up turn answer)
 *   {"type":"log","message":"..."}
 */
#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "core.h"
#include <stdbool.h>
#include <unistd.h>

/* ── Subagent events ────────────────────────────────────────────── */

typedef enum {
    SUBAGENT_EVENT_LOG = 0,     /* Progress / status text */
    SUBAGENT_EVENT_UPDATE = 1,  /* Answer from a follow-up turn */
} subagent_event_type_t;

typedef void (*subagent_event_fn)(subagent_event_type_t type,
                                  const char *text, void *ctx);

/* ── Subagent configuration ──────────────────────────────────────── */

typedef struct {
    char *provider;         /* "deepseek", "openai", "anthropic" */
    char *model;
    char *api_key;
    char *base_url;
    char *system_prompt;
    char *task;             /* The task description for the subagent */
    char *binary_path;      /* Path to cgent binary (NULL = /proc/self/exe) */
    double temperature;
    int max_tokens;
    int timeout_seconds;    /* 0 = no timeout */
    bool verbose;
    /* Optional event callback for progress logs / follow-up updates */
    subagent_event_fn on_event;
    void *event_ctx;
} subagent_config_t;

/* ── Subagent result ─────────────────────────────────────────────── */

typedef struct {
    int exit_code;
    char *output;           /* Final response text */
    char *error;            /* Error message, if any */
    bool timed_out;
    double wall_time_seconds;
} subagent_result_t;

/* ── Subagent API ────────────────────────────────────────────────── */

/* Run a subagent synchronously (blocks until complete).
 * The parent process sends the task, proxies tool calls, and
 * returns the final result.
 * Returns malloc'd result, caller frees with subagent_result_free(). */
subagent_result_t *subagent_run(subagent_config_t *config);

/* ── Async subagent handle ──────────────────────────────────────── */

/* Opaque handle to a running subagent. */
typedef struct subagent_handle subagent_handle_t;

/* Spawn a subagent without blocking. Returns a handle (or NULL with
 * *err set). The first-turn answer arrives via on_event(UPDATE). */
subagent_handle_t *subagent_spawn(subagent_config_t *config, char **err);

/* Process pending messages from the child. Blocks up to timeout_ms.
 * Returns the number of messages processed, or -1 on EOF. */
int subagent_poll(subagent_handle_t *h, int timeout_ms);

/* Send a follow-up instruction; the child answers with an UPDATE event. */
int subagent_followup(subagent_handle_t *h, const char *content);

/* Ask the child to finish after its current turn. */
int subagent_stop(subagent_handle_t *h);

/* Block until the subagent finishes; returns a malloc'd result. */
subagent_result_t *subagent_wait(subagent_handle_t *h, int timeout_seconds);

/* Kill the subagent immediately. */
void subagent_abort(subagent_handle_t *h);

/* Free a handle (after wait or abort). */
void subagent_handle_free(subagent_handle_t *h);

/* Free a subagent result */
void subagent_result_free(subagent_result_t *result);

/* ── Subagent mode (child process) ───────────────────────────────── */

/* Entry point for --subagent mode. Reads task from stdin,
 * runs the agent loop, and sends results to stdout. */
int subagent_main(int argc, char **argv);

#endif /* SUBAGENT_H */
