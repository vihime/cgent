/*
 * tool_executor.c — Tool execution with timeout
 */
#include "tools.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Approval hooks (set by the host application) ───────────────── */

static tool_approval_fn g_approval_fn = NULL;
static void *g_approval_ctx = NULL;
static tool_confirm_fn g_confirm_fn = NULL;
static void *g_confirm_ctx = NULL;

void tool_set_approval_callback(tool_approval_fn fn, void *ctx) {
    g_approval_fn = fn;
    g_approval_ctx = ctx;
}

void tool_set_confirm_callback(tool_confirm_fn fn, void *ctx) {
    g_confirm_fn = fn;
    g_confirm_ctx = ctx;
}

/* Exposed to builtin handlers (e.g. the confirm tool). */
bool tool_confirm_available(void);
bool tool_confirm_ask(const char *question);

bool tool_confirm_available(void) { return g_confirm_fn != NULL; }

bool tool_confirm_ask(const char *question) {
    if (!g_confirm_fn) return false;
    return g_confirm_fn(question, g_confirm_ctx);
}

char *tool_execute(const char *name, const char *args_json,
                   int timeout_ms, char **error) {
    (void)timeout_ms; /* TODO: implement timeout via alarm/sigaction */

    tool_t *tool = tool_registry_find(name);
    if (!tool) {
        if (error) *error = strdup("Tool not found");
        return NULL;
    }

    if (!tool->handler) {
        if (error) *error = strdup("Tool has no handler");
        return NULL;
    }

    /* Require user approval for risky tools when a hook is installed */
    if (tool->requires_approval && g_approval_fn) {
        if (!g_approval_fn(name, args_json, g_approval_ctx)) {
            if (error) *error = strdup("Tool execution denied by user (approval required)");
            return NULL;
        }
    }

    char *result = tool->handler(name, args_json, error);
    return result;
}
