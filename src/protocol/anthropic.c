/*
 * anthropic.c — Anthropic Messages API provider (stub)
 *
 * API: POST https://api.anthropic.com/v1/messages
 * Uses a different format from OpenAI/DeepSeek:
 *   - System prompt is a top-level "system" array, not in messages
 *   - Tool use is via content blocks with type="tool_use"
 *   - Streaming uses content_block_start/delta/stop events
 *
 * This is a stub — will be fully implemented post-MVP.
 */
#include "protocol.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *anthropic_build_request(const agent_t *agent) {
    json_value_t *root = json_object();
    json_object_set(root, "model",
        json_string(agent->provider.model ? agent->provider.model : "claude-sonnet-4-6"));
    json_object_set(root, "max_tokens", json_number(agent->provider.max_tokens));
    json_object_set(root, "stream", json_bool(agent->provider.stream));

    /* System prompt as top-level array */
    if (agent->system_prompt) {
        json_value_t *sys_arr = json_array();
        json_value_t *sys_block = json_object();
        json_object_set(sys_block, "type", json_string("text"));
        json_object_set(sys_block, "text", json_string(agent->system_prompt));
        json_array_append(sys_arr, sys_block);
        json_object_set(root, "system", sys_arr);
    }

    /* Messages */
    json_value_t *msgs = json_array();
    for (int i = 0; i < agent->n_messages; i++) {
        const message_t *msg = &agent->messages[i];
        json_value_t *m = json_object();
        json_object_set(m, "role", json_string(
            msg->role == MSG_ROLE_USER ? "user" : "assistant"));

        json_value_t *content = json_array();
        if (msg->content) {
            json_value_t *block = json_object();
            json_object_set(block, "type", json_string("text"));
            json_object_set(block, "text", json_string(msg->content));
            json_array_append(content, block);
        }
        /* Tool calls become tool_use blocks */
        for (int j = 0; j < msg->n_tool_calls; j++) {
            json_value_t *block = json_object();
            json_object_set(block, "type", json_string("tool_use"));
            json_object_set(block, "id", json_string(msg->tool_calls[j].id));
            json_object_set(block, "name", json_string(msg->tool_calls[j].name));
            json_value_t *input = json_parse(msg->tool_calls[j].arguments);
            json_object_set(block, "input", input ? input : json_object());
            json_array_append(content, block);
        }
        /* Tool results become tool_result blocks */
        for (int j = 0; j < msg->n_tool_results; j++) {
            json_value_t *block = json_object();
            json_object_set(block, "type", json_string("tool_result"));
            json_object_set(block, "tool_use_id",
                json_string(msg->tool_results[j].tool_call_id));
            json_object_set(block, "content",
                json_string(msg->tool_results[j].content));
            json_array_append(content, block);
        }
        json_object_set(m, "content", content);
        json_array_append(msgs, m);
    }
    json_object_set(root, "messages", msgs);

    /* Tools (Anthropic format) */
    if (agent->n_tools > 0) {
        json_value_t *tools_arr = json_array();
        for (int i = 0; i < agent->n_tools; i++) {
            json_value_t *t = json_object();
            json_object_set(t, "name", json_string(agent->tools[i].name));
            json_object_set(t, "description",
                json_string(agent->tools[i].description));
            json_value_t *params = json_parse(agent->tools[i].parameters_schema);
            json_object_set(t, "input_schema", params ? params : json_object());
            json_array_append(tools_arr, t);
        }
        json_object_set(root, "tools", tools_arr);
    }

    char *result = json_stringify(root);
    json_free(root);
    return result;
}

static message_t *anthropic_parse_response(const char *body) {
    (void)body;
    return message_create(MSG_ROLE_ASSISTANT, "[Anthropic provider stub]");
}

static message_t *anthropic_parse_chunk(const char *sse_data) {
    (void)sse_data;
    return NULL;
}

static char *anthropic_format_tool_results(const tool_result_t *results, int count) {
    json_value_t *arr = json_array();
    for (int i = 0; i < count; i++) {
        json_value_t *tr = json_object();
        json_object_set(tr, "type", json_string("tool_result"));
        json_object_set(tr, "tool_use_id", json_string(results[i].tool_call_id));
        json_object_set(tr, "content", json_string(results[i].content));
        json_array_append(arr, tr);
    }
    char *result = json_stringify(arr);
    json_free(arr);
    return result;
}

static tool_call_t *anthropic_extract_tool_calls(const char *body, int *count) {
    *count = 0;
    (void)body;
    return NULL;
}

api_provider_t provider_anthropic = {
    .type              = PROVIDER_ANTHROPIC,
    .name              = "anthropic",
    .default_base_url  = "https://api.anthropic.com",
    .default_model     = "claude-sonnet-4-6",
    .auth_header       = "x-api-key",
    .auth_prefix       = "",
    .build_request     = anthropic_build_request,
    .parse_response    = anthropic_parse_response,
    .parse_chunk       = anthropic_parse_chunk,
    .format_tool_results = anthropic_format_tool_results,
    .extract_tool_calls  = anthropic_extract_tool_calls,
};
