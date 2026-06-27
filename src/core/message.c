/*
 * message.c — Message construction and lifecycle
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

message_t *message_create(message_role_t role, const char *content) {
    message_t *msg = calloc(1, sizeof(message_t));
    if (!msg) return NULL;
    msg->role = role;
    if (content) msg->content = strdup(content);
    return msg;
}

void message_free(message_t *msg) {
    if (!msg) return;
    message_clear(msg);
    free(msg);
}

void message_clear(message_t *msg) {
    if (!msg) return;
    free(msg->content);
    msg->content = NULL;
    free(msg->name);
    msg->name = NULL;
    for (int i = 0; i < msg->n_tool_calls; i++) {
        free(msg->tool_calls[i].id);
        free(msg->tool_calls[i].name);
        free(msg->tool_calls[i].arguments);
    }
    free(msg->tool_calls);
    msg->tool_calls = NULL;
    msg->n_tool_calls = 0;
    for (int i = 0; i < msg->n_tool_results; i++) {
        free(msg->tool_results[i].tool_call_id);
        free(msg->tool_results[i].content);
    }
    free(msg->tool_results);
    msg->tool_results = NULL;
    msg->n_tool_results = 0;
}

message_t *message_copy(const message_t *msg) {
    if (!msg) return NULL;
    message_t *copy = calloc(1, sizeof(message_t));
    if (!copy) return NULL;
    copy->role = msg->role;
    if (msg->content) copy->content = strdup(msg->content);
    if (msg->name) copy->name = strdup(msg->name);

    /* Deep copy tool calls */
    if (msg->n_tool_calls > 0) {
        copy->n_tool_calls = msg->n_tool_calls;
        copy->tool_calls = calloc(msg->n_tool_calls, sizeof(tool_call_t));
        for (int i = 0; i < msg->n_tool_calls; i++) {
            copy->tool_calls[i].id = msg->tool_calls[i].id ? strdup(msg->tool_calls[i].id) : NULL;
            copy->tool_calls[i].name = msg->tool_calls[i].name ? strdup(msg->tool_calls[i].name) : NULL;
            copy->tool_calls[i].arguments = msg->tool_calls[i].arguments ? strdup(msg->tool_calls[i].arguments) : NULL;
        }
    }

    /* Deep copy tool results */
    if (msg->n_tool_results > 0) {
        copy->n_tool_results = msg->n_tool_results;
        copy->tool_results = calloc(msg->n_tool_results, sizeof(tool_result_t));
        for (int i = 0; i < msg->n_tool_results; i++) {
            copy->tool_results[i].tool_call_id = msg->tool_results[i].tool_call_id ? strdup(msg->tool_results[i].tool_call_id) : NULL;
            copy->tool_results[i].content = msg->tool_results[i].content ? strdup(msg->tool_results[i].content) : NULL;
            copy->tool_results[i].is_error = msg->tool_results[i].is_error;
        }
    }

    return copy;
}

void message_add_tool_call(message_t *msg, const char *id,
                           const char *name, const char *args) {
    if (!msg) return;
    msg->n_tool_calls++;
    msg->tool_calls = realloc(msg->tool_calls,
                              msg->n_tool_calls * sizeof(tool_call_t));
    tool_call_t *tc = &msg->tool_calls[msg->n_tool_calls - 1];
    tc->id = id ? strdup(id) : NULL;
    tc->name = name ? strdup(name) : NULL;
    tc->arguments = args ? strdup(args) : NULL;
}

void message_add_tool_result(message_t *msg, const char *call_id,
                             const char *content, bool is_error) {
    if (!msg) return;
    msg->n_tool_results++;
    msg->tool_results = realloc(msg->tool_results,
                                msg->n_tool_results * sizeof(tool_result_t));
    tool_result_t *tr = &msg->tool_results[msg->n_tool_results - 1];
    tr->tool_call_id = call_id ? strdup(call_id) : NULL;
    tr->content = content ? strdup(content) : NULL;
    tr->is_error = is_error;
}
