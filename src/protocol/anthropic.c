/*
 * anthropic.c — Anthropic Messages API provider
 *
 * API: POST https://api.anthropic.com/v1/messages
 * Uses Anthropic's native message format:
 *   - System prompt is a top-level "system" string/array
 *   - Assistant tool calls use content blocks with type="tool_use"
 *   - Tool results are user messages containing type="tool_result"
 *   - Streaming uses message_start/content_block_start/content_block_delta
 *     and message_delta events
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
    json_object_set(root, "temperature", json_number(agent->provider.temperature));

    /* System prompt as a top-level string. Anthropic also accepts an array
     * of content blocks; a string is simpler and covers cgent's text-only
     * system prompt model. */
    if (agent->system_prompt) {
        json_object_set(root, "system", json_string(agent->system_prompt));
    }

    /* Messages */
    json_value_t *msgs = json_array();
    for (int i = 0; i < agent->n_messages; i++) {
        const message_t *msg = &agent->messages[i];
        json_value_t *m = json_object();

        if (msg->role == MSG_ROLE_SYSTEM) {
            /* The configured system prompt already carries the top-level
             * system context; preserve any extra system message as a user
             * turn rather than dropping it. */
            json_object_set(m, "role", json_string("user"));
        } else if (msg->role == MSG_ROLE_TOOL) {
            /* Anthropic has no separate "tool" role: tool results are a
             * user message whose content is one or more tool_result blocks. */
            json_object_set(m, "role", json_string("user"));
        } else if (msg->role == MSG_ROLE_USER) {
            json_object_set(m, "role", json_string("user"));
        } else {
            json_object_set(m, "role", json_string("assistant"));
        }

        json_value_t *content = json_array();
        if (msg->content && msg->content[0]) {
            json_value_t *block = json_object();
            json_object_set(block, "type", json_string("text"));
            json_object_set(block, "text", json_string(msg->content));
            json_array_append(content, block);
        }

        /* Assistant tool calls become tool_use blocks. */
        if (msg->role == MSG_ROLE_ASSISTANT) {
            for (int j = 0; j < msg->n_tool_calls; j++) {
                json_value_t *block = json_object();
                json_object_set(block, "type", json_string("tool_use"));
                json_object_set(block, "id",
                    json_string(msg->tool_calls[j].id ? msg->tool_calls[j].id : ""));
                json_object_set(block, "name",
                    json_string(msg->tool_calls[j].name ? msg->tool_calls[j].name : ""));
                json_value_t *input = msg->tool_calls[j].arguments
                                      ? json_parse(msg->tool_calls[j].arguments)
                                      : NULL;
                json_object_set(block, "input", input ? input : json_object());
                json_array_append(content, block);
            }
        }

        /* Tool result messages must be sent as user tool_result blocks. */
        if (msg->role == MSG_ROLE_TOOL || msg->n_tool_results > 0) {
            for (int j = 0; j < msg->n_tool_results; j++) {
                json_value_t *block = json_object();
                json_object_set(block, "type", json_string("tool_result"));
                json_object_set(block, "tool_use_id",
                    json_string(msg->tool_results[j].tool_call_id
                               ? msg->tool_results[j].tool_call_id : ""));
                json_object_set(block, "content",
                    json_string(msg->tool_results[j].content
                               ? msg->tool_results[j].content : ""));
                if (msg->tool_results[j].is_error) {
                    json_object_set(block, "is_error", json_bool(true));
                }
                json_array_append(content, block);
            }
        }

        /* Anthropic requires a non-empty content array. */
        if (json_array_length(content) == 0) {
            json_value_t *block = json_object();
            json_object_set(block, "type", json_string("text"));
            json_object_set(block, "text", json_string(""));
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

/* Append text to a heap-allocated string, reallocating as needed. */
static void anthropic_append_text(char **buf, const char *text) {
    if (!buf || !text || !text[0]) return;
    size_t old_len = *buf ? strlen(*buf) : 0;
    size_t add_len = strlen(text);
    char *grown = realloc(*buf, old_len + add_len + 1);
    if (!grown) return;
    memcpy(grown + old_len, text, add_len);
    grown[old_len + add_len] = '\0';
    *buf = grown;
}

static message_t *anthropic_parse_response(const char *body) {
    json_value_t *root = json_parse(body);
    if (!root) {
        fprintf(stderr, "[anthropic] JSON parse failed. Body starts: %.200s\n",
                body ? body : "(null)");
        return NULL;
    }

    json_value_t *error = json_object_get(root, "error");
    if (error && json_is_object(error)) {
        json_value_t *err_msg = json_object_get(error, "message");
        fprintf(stderr, "[anthropic] API returned error: %s\n",
                err_msg && json_is_string(err_msg)
                    ? json_string_value(err_msg) : "unknown");
        json_free(root);
        return NULL;
    }

    message_t *msg = message_create(MSG_ROLE_ASSISTANT, NULL);
    if (!msg) { json_free(root); return NULL; }

    json_value_t *content = json_object_get(root, "content");
    if (content && json_is_array(content)) {
        int n = json_array_length(content);
        for (int i = 0; i < n; i++) {
            json_value_t *block = json_array_get(content, i);
            if (!block || !json_is_object(block)) continue;

            json_value_t *type = json_object_get(block, "type");
            const char *typestr = type && json_is_string(type)
                                  ? json_string_value(type) : "";

            if (strcmp(typestr, "text") == 0) {
                json_value_t *text = json_object_get(block, "text");
                if (text && json_is_string(text))
                    anthropic_append_text(&msg->content, json_string_value(text));
            } else if (strcmp(typestr, "thinking") == 0) {
                json_value_t *text = json_object_get(block, "thinking");
                if (text && json_is_string(text))
                    anthropic_append_text(&msg->reasoning_content,
                                          json_string_value(text));
            } else if (strcmp(typestr, "tool_use") == 0) {
                json_value_t *id = json_object_get(block, "id");
                json_value_t *name = json_object_get(block, "name");
                json_value_t *input = json_object_get(block, "input");
                char *input_str = input ? json_stringify(input) : strdup("{}");
                if (input_str) {
                    message_add_tool_call(msg,
                        id && json_is_string(id) ? json_string_value(id) : "",
                        name && json_is_string(name) ? json_string_value(name) : "",
                        input_str);
                    free(input_str);
                }
            }
        }
    }

    /* Anthropic uses input_tokens/output_tokens instead of prompt/completion. */
    json_value_t *usage = json_object_get(root, "usage");
    if (usage && json_is_object(usage)) {
        json_value_t *it = json_object_get(usage, "input_tokens");
        json_value_t *ot = json_object_get(usage, "output_tokens");
        if (it && json_is_number(it))
            msg->prompt_tokens = (long long)json_number_value(it);
        if (ot && json_is_number(ot))
            msg->completion_tokens = (long long)json_number_value(ot);
    }

    json_free(root);
    return msg;
}

static message_t *anthropic_parse_chunk(const char *sse_data) {
    if (!sse_data || !sse_data[0] || strcmp(sse_data, "[DONE]") == 0)
        return NULL;

    json_value_t *root = json_parse(sse_data);
    if (!root) return NULL;

    message_t *delta = message_create(MSG_ROLE_ASSISTANT, NULL);
    if (!delta) { json_free(root); return NULL; }

    json_value_t *type = json_object_get(root, "type");
    const char *typestr = type && json_is_string(type)
                          ? json_string_value(type) : "";

    if (strcmp(typestr, "message_start") == 0) {
        json_value_t *message = json_object_get(root, "message");
        if (message && json_is_object(message)) {
            json_value_t *usage = json_object_get(message, "usage");
            if (usage && json_is_object(usage)) {
                json_value_t *it = json_object_get(usage, "input_tokens");
                if (it && json_is_number(it))
                    delta->prompt_tokens = (long long)json_number_value(it);
            }
        }
    } else if (strcmp(typestr, "content_block_start") == 0) {
        json_value_t *block = json_object_get(root, "content_block");
        if (block && json_is_object(block)) {
            json_value_t *btype = json_object_get(block, "type");
            const char *btypestr = btype && json_is_string(btype)
                                   ? json_string_value(btype) : "";
            if (strcmp(btypestr, "text") == 0) {
                json_value_t *text = json_object_get(block, "text");
                if (text && json_is_string(text) && json_string_value(text)[0])
                    delta->content = strdup(json_string_value(text));
            } else if (strcmp(btypestr, "thinking") == 0) {
                json_value_t *text = json_object_get(block, "thinking");
                if (text && json_is_string(text))
                    delta->reasoning_content = strdup(json_string_value(text));
            } else if (strcmp(btypestr, "tool_use") == 0) {
                json_value_t *id = json_object_get(block, "id");
                json_value_t *name = json_object_get(block, "name");
                json_value_t *input = json_object_get(block, "input");
                /* Usually input is an empty object here and the real JSON
                 * arrives as input_json_delta fragments. Only emit non-empty
                 * input now so the stream merger can accumulate deltas. */
                char *input_str = NULL;
                if (input && json_is_object(input) && json_object_size(input) > 0)
                    input_str = json_stringify(input);
                message_add_tool_call(delta,
                    id && json_is_string(id) ? json_string_value(id) : "",
                    name && json_is_string(name) ? json_string_value(name) : "",
                    input_str);
                free(input_str);
            }
        }
    } else if (strcmp(typestr, "content_block_delta") == 0) {
        json_value_t *d = json_object_get(root, "delta");
        if (d && json_is_object(d)) {
            json_value_t *dtype = json_object_get(d, "type");
            const char *dtypestr = dtype && json_is_string(dtype)
                                   ? json_string_value(dtype) : "";
            if (strcmp(dtypestr, "text_delta") == 0) {
                json_value_t *text = json_object_get(d, "text");
                if (text && json_is_string(text))
                    delta->content = strdup(json_string_value(text));
            } else if (strcmp(dtypestr, "thinking_delta") == 0) {
                json_value_t *text = json_object_get(d, "thinking");
                if (text && json_is_string(text))
                    delta->reasoning_content = strdup(json_string_value(text));
            } else if (strcmp(dtypestr, "input_json_delta") == 0) {
                json_value_t *partial = json_object_get(d, "partial_json");
                if (partial && json_is_string(partial) &&
                    json_string_value(partial)[0]) {
                    message_add_tool_call(delta, "", "",
                                          json_string_value(partial));
                }
            }
        }
    } else if (strcmp(typestr, "message_delta") == 0) {
        json_value_t *usage = json_object_get(root, "usage");
        if (usage && json_is_object(usage)) {
            json_value_t *ot = json_object_get(usage, "output_tokens");
            if (ot && json_is_number(ot))
                delta->completion_tokens = (long long)json_number_value(ot);
        }
    }

    json_free(root);
    return delta;
}

static char *anthropic_format_tool_results(const tool_result_t *results, int count) {
    json_value_t *arr = json_array();
    for (int i = 0; i < count; i++) {
        json_value_t *tr = json_object();
        json_object_set(tr, "type", json_string("tool_result"));
        json_object_set(tr, "tool_use_id",
            json_string(results[i].tool_call_id ? results[i].tool_call_id : ""));
        json_object_set(tr, "content",
            json_string(results[i].content ? results[i].content : ""));
        json_array_append(arr, tr);
    }
    char *result = json_stringify(arr);
    json_free(arr);
    return result;
}

static tool_call_t *anthropic_extract_tool_calls(const char *body, int *count) {
    *count = 0;
    json_value_t *root = json_parse(body);
    if (!root) return NULL;

    tool_call_t *calls = NULL;
    json_value_t *content = json_object_get(root, "content");
    if (content && json_is_array(content)) {
        int n = json_array_length(content);
        for (int i = 0; i < n; i++) {
            json_value_t *block = json_array_get(content, i);
            if (!block || !json_is_object(block)) continue;

            json_value_t *type = json_object_get(block, "type");
            if (!type || !json_is_string(type) ||
                strcmp(json_string_value(type), "tool_use") != 0)
                continue;

            tool_call_t *grown = realloc(calls, (*count + 1) * sizeof(tool_call_t));
            if (!grown) {
                for (int j = 0; j < *count; j++) {
                    free(calls[j].id);
                    free(calls[j].name);
                    free(calls[j].arguments);
                }
                free(calls);
                json_free(root);
                return NULL;
            }
            calls = grown;
            memset(&calls[*count], 0, sizeof(tool_call_t));

            json_value_t *id = json_object_get(block, "id");
            json_value_t *name = json_object_get(block, "name");
            json_value_t *input = json_object_get(block, "input");
            calls[*count].id = (id && json_is_string(id))
                               ? strdup(json_string_value(id)) : strdup("");
            calls[*count].name = (name && json_is_string(name))
                                 ? strdup(json_string_value(name)) : strdup("");
            calls[*count].arguments = input ? json_stringify(input) : strdup("{}");
            (*count)++;
        }
    }

    json_free(root);
    return calls;
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
