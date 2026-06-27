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
 *
 * IPC Protocol (child → parent, over child's stdout):
 *   {"type":"tool_call","id":"call_xxx","name":"...","arguments":"..."}
 *   {"type":"result","content":"...","error":null}
 *   {"type":"log","message":"..."}
 */
#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "core.h"
#include <stdbool.h>
#include <unistd.h>

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

/* Free a subagent result */
void subagent_result_free(subagent_result_t *result);

/* ── Subagent mode (child process) ───────────────────────────────── */

/* Entry point for --subagent mode. Reads task from stdin,
 * runs the agent loop, and sends results to stdout. */
int subagent_main(int argc, char **argv);

#endif /* SUBAGENT_H */
