/*
 * openai.c — OpenAI Chat Completions API provider
 *
 * Same format as DeepSeek (OpenAI-compatible).
 * Differentiated by base URL and model names.
 * API: POST https://api.openai.com/v1/chat/completions
 */
#include "protocol.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Reuses the same format as DeepSeek */
static char *openai_build_request(const agent_t *agent) {
    /* Identical JSON structure to DeepSeek */
    json_value_t *root = json_object();
    if (!root) return NULL;

    json_object_set(root, "model",
        json_string(agent->provider.model ? agent->provider.model : "gpt-4o"));

    json_value_t *msgs = json_array();

    if (agent->system_prompt && agent->system_prompt[0]) {
        json_value_t *sys = json_object();
        json_object_set(sys, "role", json_string("system"));
        json_object_set(sys, "content", json_string(agent->system_prompt));
        json_array_append(msgs, sys);
    }

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
            if (msg->content && msg->content[0])
                json_object_set(m, "content", json_string(msg->content));
            else if (msg->n_tool_calls > 0)
                json_object_set(m, "content", json_null());
            if (msg->n_tool_calls > 0) {
                json_value_t *tcs = json_array();
                for (int j = 0; j < msg->n_tool_calls; j++) {
                    json_value_t *tc = json_object();
                    json_object_set(tc, "id", json_string(msg->tool_calls[j].id));
                    json_object_set(tc, "type", json_string("function"));
                    json_value_t *func = json_object();
                    json_object_set(func, "name", json_string(msg->tool_calls[j].name));
                    json_object_set(func, "arguments", json_string(msg->tool_calls[j].arguments));
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

    if (agent->n_tools > 0) {
        json_value_t *tools_arr = json_array();
        for (int i = 0; i < agent->n_tools; i++) {
            const tool_t *tool = &agent->tools[i];
            json_value_t *t = json_object();
            json_object_set(t, "type", json_string("function"));
            json_value_t *func = json_object();
            json_object_set(func, "name", json_string(tool->name));
            json_object_set(func, "description", json_string(tool->description));
            json_value_t *params = json_parse(tool->parameters_schema);
            json_object_set(func, "parameters", params ? params : json_object());
            json_object_set(t, "function", func);
            json_array_append(tools_arr, t);
        }
        json_object_set(root, "tools", tools_arr);
    }

    json_object_set(root, "stream", json_bool(agent->provider.stream));
    json_object_set(root, "temperature", json_number(agent->provider.temperature));
    json_object_set(root, "max_tokens", json_number(agent->provider.max_tokens));

    char *result = json_stringify(root);
    json_free(root);
    return result;
}

/* Same response format as DeepSeek */
static message_t *openai_parse_response(const char *body) {
    extern api_provider_t provider_deepseek;
    return provider_deepseek.parse_response(body);
}

static message_t *openai_parse_chunk(const char *sse_data) {
    extern api_provider_t provider_deepseek;
    return provider_deepseek.parse_chunk(sse_data);
}

static char *openai_format_tool_results(const tool_result_t *results, int count) {
    extern api_provider_t provider_deepseek;
    return provider_deepseek.format_tool_results(results, count);
}

static tool_call_t *openai_extract_tool_calls(const char *body, int *count) {
    extern api_provider_t provider_deepseek;
    return provider_deepseek.extract_tool_calls(body, count);
}

api_provider_t provider_openai = {
    .type              = PROVIDER_OPENAI,
    .name              = "openai",
    .default_base_url  = "https://api.openai.com",
    .default_model     = "gpt-4o",
    .auth_header       = "Authorization",
    .auth_prefix       = "Bearer ",
    .build_request     = openai_build_request,
    .parse_response    = openai_parse_response,
    .parse_chunk       = openai_parse_chunk,
    .format_tool_results = openai_format_tool_results,
    .extract_tool_calls  = openai_extract_tool_calls,
};
