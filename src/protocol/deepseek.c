/*
 * deepseek.c — DeepSeek Chat Completions API provider
 *
 * API: POST https://api.deepseek.com/v1/chat/completions
 * Format: OpenAI-compatible chat completions
 *
 * Request:
 * {
 *   "model": "deepseek-chat",
 *   "messages": [
 *     {"role": "system", "content": "..."},
 *     {"role": "user", "content": "..."}
 *   ],
 *   "tools": [{"type": "function", "function": {...}}],
 *   "stream": true|false,
 *   "temperature": 0.7,
 *   "max_tokens": 4096
 * }
 *
 * Response (non-streaming):
 * {
 *   "choices": [{"message": {"role": "assistant", "content": "...",
 *     "tool_calls": [{"id": "...", "type": "function",
 *       "function": {"name": "...", "arguments": "..."}}]}}]
 * }
 *
 * Streaming delta:
 *   "choices": [{"delta": {"content": "...",
 *     "tool_calls": [{"index": 0, "id": "...",
 *       "function": {"name": "...", "arguments": "..."}}]}}]
 */
#include "protocol.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Build request ──────────────────────────────────────────────── */

static char *deepseek_build_request(const agent_t *agent) {
    json_value_t *root = json_object();
    if (!root) return NULL;

    /* Model */
    json_object_set(root, "model",
        json_string(agent->provider.model ? agent->provider.model : "deepseek-chat"));

    /* Messages array */
    json_value_t *msgs = json_array();

    /* System prompt */
    if (agent->system_prompt && agent->system_prompt[0]) {
        json_value_t *sys = json_object();
        json_object_set(sys, "role", json_string("system"));
        json_object_set(sys, "content", json_string(agent->system_prompt));
        json_array_append(msgs, sys);
    }

    /* Conversation messages */
    for (int i = 0; i < agent->n_messages; i++) {
        const message_t *msg = &agent->messages[i];
        json_value_t *m = json_object();

        switch (msg->role) {
        case MSG_ROLE_SYSTEM:
            json_object_set(m, "role", json_string("system"));
            if (msg->content)
                json_object_set(m, "content", json_string(msg->content));
            break;
        case MSG_ROLE_USER:
            json_object_set(m, "role", json_string("user"));
            if (msg->content)
                json_object_set(m, "content", json_string(msg->content));
            break;
        case MSG_ROLE_ASSISTANT:
            json_object_set(m, "role", json_string("assistant"));
            /* Always set content — null when tool calls are present */
            if (msg->content && msg->content[0])
                json_object_set(m, "content", json_string(msg->content));
            else if (msg->n_tool_calls > 0)
                json_object_set(m, "content", json_null());
            /* Tool calls */
            if (msg->n_tool_calls > 0) {
                json_value_t *tcs = json_array();
                for (int j = 0; j < msg->n_tool_calls; j++) {
                    json_value_t *tc = json_object();
                    json_object_set(tc, "id", json_string(msg->tool_calls[j].id));
                    json_object_set(tc, "type", json_string("function"));
                    json_value_t *func = json_object();
                    json_object_set(func, "name",
                        json_string(msg->tool_calls[j].name));
                    json_object_set(func, "arguments",
                        json_string(msg->tool_calls[j].arguments));
                    json_object_set(tc, "function", func);
                    json_array_append(tcs, tc);
                }
                json_object_set(m, "tool_calls", tcs);
            }
            break;
        case MSG_ROLE_TOOL:
            json_object_set(m, "role", json_string("tool"));
            if (msg->n_tool_results > 0) {
                json_object_set(m, "tool_call_id",
                    json_string(msg->tool_results[0].tool_call_id));
                json_object_set(m, "content",
                    json_string(msg->tool_results[0].content));
            }
            break;
        }
        json_array_append(msgs, m);
    }
    json_object_set(root, "messages", msgs);

    /* Tools */
    if (agent->n_tools > 0) {
        json_value_t *tools_arr = json_array();
        for (int i = 0; i < agent->n_tools; i++) {
            const tool_t *tool = &agent->tools[i];
            json_value_t *t = json_object();
            json_object_set(t, "type", json_string("function"));
            json_value_t *func = json_object();
            json_object_set(func, "name", json_string(tool->name));
            json_object_set(func, "description", json_string(tool->description));

            /* Parse parameters schema from JSON string */
            json_value_t *params = json_parse(tool->parameters_schema);
            json_object_set(func, "parameters", params ? params : json_object());
            json_object_set(t, "function", func);
            json_array_append(tools_arr, t);
        }
        json_object_set(root, "tools", tools_arr);
    }

    /* Other parameters */
    json_object_set(root, "stream", json_bool(agent->provider.stream));
    json_object_set(root, "temperature",
        json_number(agent->provider.temperature));
    json_object_set(root, "max_tokens",
        json_number(agent->provider.max_tokens));

    char *result = json_stringify(root);
    json_free(root);
    return result;
}

/* ── Parse response ─────────────────────────────────────────────── */

static message_t *deepseek_parse_response(const char *body) {
    json_value_t *root = json_parse(body);
    if (!root) {
        fprintf(stderr, "[deepseek] JSON parse failed. Body starts: %.200s\n",
                body ? body : "(null)");
        return NULL;
    }

    /* Validate response structure */
    if (!json_is_object(root)) {
        fprintf(stderr, "[deepseek] Response is not a JSON object\n");
        json_free(root);
        return NULL;
    }

    message_t *msg = message_create(MSG_ROLE_ASSISTANT, NULL);
    if (!msg) { json_free(root); return NULL; }

    /* Check for API-level error */
    json_value_t *api_error = json_object_get(root, "error");
    if (api_error && json_is_object(api_error)) {
        json_value_t *err_msg = json_object_get(api_error, "message");
        fprintf(stderr, "[deepseek] API returned error: %s\n",
                err_msg && json_is_string(err_msg) ? json_string_value(err_msg) : "unknown");
        json_free(root);
        message_free(msg);
        return NULL;
    }

    /* Extract choices[0].message */
    json_value_t *choices = json_object_get(root, "choices");
    if (!choices || !json_is_array(choices)) {
        fprintf(stderr, "[deepseek] No 'choices' array in response\n");
        json_free(root);
        message_free(msg);
        return NULL;
    }

    if (json_array_length(choices) == 0) {
        fprintf(stderr, "[deepseek] 'choices' array is empty\n");
        json_free(root);
        message_free(msg);
        return NULL;
    }

    json_value_t *choice = json_array_get(choices, 0);
    json_value_t *message = json_object_get(choice, "message");

    if (!message || !json_is_object(message)) {
        fprintf(stderr, "[deepseek] No 'message' object in choice\n");
        json_free(root);
        message_free(msg);
        return NULL;
    }

    /* Content */
    json_value_t *content = json_object_get(message, "content");
    if (content && json_is_string(content)) {
        msg->content = strdup(json_string_value(content));
    }

    /* Tool calls */
    json_value_t *tool_calls = json_object_get(message, "tool_calls");
    if (tool_calls && json_is_array(tool_calls)) {
        int n = json_array_length(tool_calls);
        for (int i = 0; i < n; i++) {
            json_value_t *tc = json_array_get(tool_calls, i);
            if (!tc || !json_is_object(tc)) continue;
            json_value_t *id = json_object_get(tc, "id");
            json_value_t *func = json_object_get(tc, "function");
            json_value_t *name = NULL;
            json_value_t *args = NULL;
            if (func && json_is_object(func)) {
                name = json_object_get(func, "name");
                args = json_object_get(func, "arguments");
            }
            message_add_tool_call(msg,
                id ? json_string_value(id) : "",
                name ? json_string_value(name) : "",
                args ? json_string_value(args) : "{}");
        }
    }

    json_free(root);
    return msg;
}

/* ── Parse streaming chunk ──────────────────────────────────────── */

static message_t *deepseek_parse_chunk(const char *sse_data) {
    if (!sse_data || *sse_data == '\0') return NULL;

    /* Check for [DONE] marker */
    if (strcmp(sse_data, "[DONE]") == 0) return NULL;

    json_value_t *root = json_parse(sse_data);
    if (!root) return NULL;

    message_t *delta = message_create(MSG_ROLE_ASSISTANT, NULL);
    if (!delta) { json_free(root); return NULL; }

    json_value_t *choices = json_object_get(root, "choices");
    if (choices && json_array_length(choices) > 0) {
        json_value_t *choice = json_array_get(choices, 0);
        json_value_t *d = json_object_get(choice, "delta");
        if (d) {
            /* Text content */
            json_value_t *content = json_object_get(d, "content");
            if (content && json_is_string(content)) {
                delta->content = strdup(json_string_value(content));
            }

            /* Tool calls */
            json_value_t *tool_calls = json_object_get(d, "tool_calls");
            if (tool_calls && json_is_array(tool_calls)) {
                int n = json_array_length(tool_calls);
                for (int i = 0; i < n; i++) {
                    json_value_t *tc = json_array_get(tool_calls, i);
                    json_value_t *id = json_object_get(tc, "id");
                    json_value_t *func = json_object_get(tc, "function");
                    json_value_t *name = func ? json_object_get(func, "name") : NULL;
                    json_value_t *args = func ? json_object_get(func, "arguments") : NULL;

                    message_add_tool_call(delta,
                        id ? json_string_value(id) : "",
                        name ? json_string_value(name) : "",
                        args ? json_string_value(args) : "");
                }
            }
        }
    }

    json_free(root);
    return delta;
}

/* ── Format tool results ────────────────────────────────────────── */

static char *deepseek_format_tool_results(const tool_result_t *results, int count) {
    /* Tool results are included as messages, not a separate array */
    json_value_t *arr = json_array();
    for (int i = 0; i < count; i++) {
        json_value_t *tr = json_object();
        json_object_set(tr, "role", json_string("tool"));
        json_object_set(tr, "tool_call_id", json_string(results[i].tool_call_id));
        json_object_set(tr, "content", json_string(results[i].content));
        json_array_append(arr, tr);
    }
    char *result = json_stringify(arr);
    json_free(arr);
    return result;
}

/* ── Extract tool calls ─────────────────────────────────────────── */

static tool_call_t *deepseek_extract_tool_calls(const char *body, int *count) {
    *count = 0;
    json_value_t *root = json_parse(body);
    if (!root) return NULL;

    tool_call_t *calls = NULL;
    json_value_t *choices = json_object_get(root, "choices");
    if (choices && json_array_length(choices) > 0) {
        json_value_t *choice = json_array_get(choices, 0);
        json_value_t *message = json_object_get(choice, "message");
        json_value_t *tool_calls = json_object_get(message, "tool_calls");

        if (tool_calls && json_is_array(tool_calls)) {
            int n = json_array_length(tool_calls);
            calls = calloc(n, sizeof(tool_call_t));
            for (int i = 0; i < n; i++) {
                json_value_t *tc = json_array_get(tool_calls, i);
                json_value_t *id = json_object_get(tc, "id");
                json_value_t *func = json_object_get(tc, "function");
                json_value_t *name = json_object_get(func, "name");
                json_value_t *args = json_object_get(func, "arguments");

                calls[i].id = id ? strdup(json_string_value(id)) : NULL;
                calls[i].name = name ? strdup(json_string_value(name)) : NULL;
                calls[i].arguments = args ? strdup(json_string_value(args)) : NULL;
                (*count)++;
            }
        }
    }

    json_free(root);
    return calls;
}

/* ── Provider definition ────────────────────────────────────────── */

api_provider_t provider_deepseek = {
    .type              = PROVIDER_DEEPSEEK,
    .name              = "deepseek",
    .default_base_url  = "https://api.deepseek.com",
    .default_model     = "deepseek-chat",
    .auth_header       = "Authorization",
    .auth_prefix       = "Bearer ",
    .build_request     = deepseek_build_request,
    .parse_response    = deepseek_parse_response,
    .parse_chunk       = deepseek_parse_chunk,
    .format_tool_results = deepseek_format_tool_results,
    .extract_tool_calls  = deepseek_extract_tool_calls,
};
