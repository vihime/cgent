/*
 * agent.c — Agent lifecycle, conversation loop, tool-use handling
 *
 * The central module that orchestrates:
 *   1. Building the API request from conversation state
 *   2. Sending the request via HTTP/TLS
 *   3. Parsing the response (non-streaming or SSE streaming)
 *   4. Executing tool calls
 *   5. Looping until the assistant produces a final text response
 */
#include "cgent.h"
#include "core.h"
#include "protocol.h"
#include "network.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Agent lifecycle ────────────────────────────────────────────── */

agent_t *agent_create(provider_config_t *config, struct api_provider *api) {
    agent_t *agent = calloc(1, sizeof(agent_t));
    if (!agent) return NULL;

    if (config) {
        agent->provider.api_key     = config->api_key ? strdup(config->api_key) : NULL;
        agent->provider.base_url    = config->base_url ? strdup(config->base_url) : NULL;
        agent->provider.model       = config->model ? strdup(config->model) : NULL;
        agent->provider.temperature = config->temperature;
        agent->provider.max_tokens  = config->max_tokens;
        agent->provider.stream      = config->stream;
    }
    agent->api = api;

    agent->cap_messages = 64;
    agent->messages = calloc(agent->cap_messages, sizeof(message_t));
    agent->n_messages = 0;

    agent->cap_tools = 32;
    agent->tools = calloc(agent->cap_tools, sizeof(tool_t));
    agent->n_tools = 0;

    return agent;
}

void agent_free(agent_t *agent) {
    if (!agent) return;
    free(agent->provider.api_key);
    free(agent->provider.base_url);
    free(agent->provider.model);
    free(agent->system_prompt);
    for (int i = 0; i < agent->n_messages; i++)
        message_clear(&agent->messages[i]);
    free(agent->messages);
    /* Tools are owned by the global registry — don't free them here */
    free(agent->tools);
    free(agent);
}

void agent_set_system_prompt(agent_t *agent, const char *prompt) {
    if (!agent) return;
    free(agent->system_prompt);
    agent->system_prompt = prompt ? strdup(prompt) : NULL;
}

int agent_add_tool(agent_t *agent, const tool_t *tool) {
    if (!agent || !tool) return -1;
    if (agent->n_tools >= agent->cap_tools) {
        agent->cap_tools *= 2;
        agent->tools = realloc(agent->tools, agent->cap_tools * sizeof(tool_t));
    }
    agent->tools[agent->n_tools] = *tool;
    agent->n_tools++;
    return 0;
}

int agent_add_message(agent_t *agent, const message_t *msg) {
    if (!agent || !msg) return -1;
    if (agent->n_messages >= agent->cap_messages) {
        agent->cap_messages *= 2;
        agent->messages = realloc(agent->messages,
                                  agent->cap_messages * sizeof(message_t));
    }
    /* Deep copy to avoid double-free issues */
    message_t *copy = message_copy(msg);
    if (!copy) return -1;
    agent->messages[agent->n_messages++] = *copy;
    free(copy); /* Free the wrapper struct, not the contents (now owned by agent) */
    return 0;
}

/* ── HTTP request helper ────────────────────────────────────────── */

/* Build the API endpoint URL from provider base URL */
static char *agent_endpoint_url(agent_t *agent) {
    const char *base = agent->provider.base_url;
    if (!base) base = agent->api->default_base_url;

    /* DeepSeek and OpenAI use chat completions endpoint */
    if (agent->api->type == PROVIDER_ANTHROPIC) {
        size_t len = strlen(base) + 16;
        char *url = malloc(len);
        snprintf(url, len, "%s/v1/messages", base);
        return url;
    } else {
        size_t len = strlen(base) + 32;
        char *url = malloc(len);
        snprintf(url, len, "%s/v1/chat/completions", base);
        return url;
    }
}

/* Build authorization header value */
static char *agent_auth_header(agent_t *agent) {
    const char *prefix = agent->api->auth_prefix ? agent->api->auth_prefix : "Bearer ";
    size_t len = strlen(prefix) + strlen(agent->provider.api_key) + 1;
    char *val = malloc(len);
    snprintf(val, len, "%s%s", prefix, agent->provider.api_key);
    return val;
}

/* ── SSE-aware response parsing ─────────────────────────────────── */

/* Parse an HTTP response body that could be either:
 *   1. Plain JSON: {"choices":[...]} (stream=false)
 *   2. SSE stream:  data: {...}\n\ndata: {...}\n\n... (stream=true)
 *
 * For SSE, accumulates all deltas via the provider's parse_chunk.
 * Calls on_token for each text delta (for streaming output).
 * Returns the full accumulated message, or NULL on failure. */
static message_t *agent_parse_response_body(api_provider_t *api,
                                             const char *body,
                                             bool verbose,
                                             void (*on_token)(const char *token, void *ctx),
                                             void *token_ctx) {
    if (!body || !body[0]) return NULL;

    /* Detect SSE format: starts with "data: " */
    if (strncmp(body, "data: ", 6) == 0) {
        if (verbose) {
            fprintf(stderr, "[agent] Detected SSE streaming response\n");
        }

        /* Accumulator message */
        message_t *accum = message_create(MSG_ROLE_ASSISTANT, NULL);
        if (!accum) return NULL;

        /* Accumulated text */
        char *text_buf = NULL;
        size_t text_len = 0;

        /* Tool call merger: SSE spreads tool call fields across chunks.
         * Track up to 16 in-progress tool calls by index, merging fields. */
        #define MAX_PENDING_TC 16
        typedef struct {
            int index;
            char *id;
            char *name;
            char *arguments;
        } pending_tc_t;
        pending_tc_t pending_tcs[MAX_PENDING_TC];
        int n_pending = 0;
        memset(pending_tcs, 0, sizeof(pending_tcs));

        /* Split by "\n\n" (SSE event boundary) */
        const char *p = body;
        while (*p) {
            /* Find next event boundary */
            const char *end = strstr(p, "\n\n");
            if (!end) end = p + strlen(p);

            /* Process lines within this event */
            const char *line_start = p;
            while (line_start < end) {
                const char *line_end = strchr(line_start, '\n');
                if (!line_end || line_end > end) line_end = end;

                size_t line_len = line_end - line_start;
                /* Ignore empty lines and comments */
                if (line_len > 0 && line_start[0] != ':') {
                    /* Check for "data: " prefix */
                    if (line_len > 6 && strncmp(line_start, "data: ", 6) == 0) {
                        const char *json_start = line_start + 6;
                        size_t json_len = line_len - 6;

                        /* Extract JSON string (not null-terminated within line) */
                        char *json_str = strndup(json_start, json_len);

                        /* Check for [DONE] marker */
                        if (strcmp(json_str, "[DONE]") == 0) {
                            free(json_str);
                            goto sse_done;
                        }

                        /* Parse this chunk via the provider */
                        message_t *delta = api->parse_chunk(json_str);

                        /* Also parse the raw JSON to extract tool_call index */
                        json_value_t *raw = json_parse(json_str);
                        int tc_index = -1;
                        if (raw) {
                            json_value_t *choices = json_object_get(raw, "choices");
                            if (choices && json_array_length(choices) > 0) {
                                json_value_t *c0 = json_array_get(choices, 0);
                                json_value_t *d = json_object_get(c0, "delta");
                                if (d) {
                                    json_value_t *tcs = json_object_get(d, "tool_calls");
                                    if (tcs && json_array_length(tcs) > 0) {
                                        json_value_t *tc0 = json_array_get(tcs, 0);
                                        json_value_t *idx = json_object_get(tc0, "index");
                                        if (idx) tc_index = (int)json_number_value(idx);
                                    }
                                }
                            }
                            json_free(raw);
                        }
                        free(json_str);

                        if (delta) {
                            /* Accumulate text content and emit via callback */
                            if (delta->content) {
                                size_t dlen = strlen(delta->content);
                                text_buf = realloc(text_buf, text_len + dlen + 1);
                                memcpy(text_buf + text_len, delta->content, dlen);
                                text_len += dlen;
                                text_buf[text_len] = '\0';
                                /* Emit this chunk immediately for streaming effect */
                                if (on_token) {
                                    on_token(delta->content, token_ctx);
                                }
                            }
                            /* Accumulate tool calls — merge by index */
                            for (int i = 0; i < delta->n_tool_calls; i++) {
                                tool_call_t *tc = &delta->tool_calls[i];

                                /* Use the index from raw JSON, or fall back to loop counter */
                                int idx = (tc_index >= 0) ? tc_index : i;

                                /* Find or create pending tool call entry */
                                pending_tc_t *pt = NULL;
                                for (int j = 0; j < n_pending; j++) {
                                    if (pending_tcs[j].index == idx) {
                                        pt = &pending_tcs[j];
                                        break;
                                    }
                                }
                                if (!pt && n_pending < MAX_PENDING_TC) {
                                    pt = &pending_tcs[n_pending++];
                                    pt->index = idx;
                                }

                                if (pt) {
                                    /* Merge fields:
                                     * - id and name: overwrite (set once in first chunk)
                                     * - arguments: CONCATENATE (streamed in fragments) */
                                    if (tc->id && tc->id[0]) {
                                        free(pt->id);
                                        pt->id = strdup(tc->id);
                                    }
                                    if (tc->name && tc->name[0]) {
                                        free(pt->name);
                                        pt->name = strdup(tc->name);
                                    }
                                    if (tc->arguments && tc->arguments[0]) {
                                        /* Concatenate argument fragments */
                                        size_t old_len = pt->arguments ? strlen(pt->arguments) : 0;
                                        size_t add_len = strlen(tc->arguments);
                                        char *merged = realloc(pt->arguments, old_len + add_len + 1);
                                        if (merged) {
                                            memcpy(merged + old_len, tc->arguments, add_len);
                                            merged[old_len + add_len] = '\0';
                                            pt->arguments = merged;
                                        }
                                    }
                                }
                            }
                            message_free(delta);
                        }
                    }
                }
                line_start = line_end + 1;
                if (line_end >= end) break;
            }

            p = (*end == '\0') ? end : end + 2; /* Skip \n\n */
        }

sse_done:
        /* Set accumulated text on the message */
        if (text_buf) {
            accum->content = text_buf;
        }

        /* Flush merged tool calls into accum — only those with an id */
        for (int i = 0; i < n_pending; i++) {
            pending_tc_t *pt = &pending_tcs[i];
            if (pt->id && pt->id[0]) {
                message_add_tool_call(accum,
                    pt->id,
                    pt->name ? pt->name : "",
                    pt->arguments ? pt->arguments : "{}");
            }
            free(pt->id);
            free(pt->name);
            free(pt->arguments);
        }
        #undef MAX_PENDING_TC

        if (verbose) {
            fprintf(stderr, "[agent] SSE accumulated: content=%s, n_tool_calls=%d\n",
                    accum->content ? accum->content : "(nil)",
                    accum->n_tool_calls);
        }

        return accum;
    }

    /* Plain JSON — use provider's standard parser */
    if (verbose) {
        fprintf(stderr, "[agent] Detected plain JSON response\n");
    }
    return api->parse_response(body);
}

/* ── Non-streaming chat ─────────────────────────────────────────── */

message_t *agent_chat(agent_t *agent, const char *user_input) {
    if (!agent || !user_input) return NULL;

    /* Add user message to conversation */
    message_t user_msg = {
        .role = MSG_ROLE_USER,
        .content = strdup(user_input),
        .n_tool_calls = 0,
        .n_tool_results = 0,
    };
    agent_add_message(agent, &user_msg);
    /* Don't free user_msg.content — it's now owned by agent->messages */

    message_t *final_response = NULL;
    int max_rounds = 10; /* Prevent infinite loops */

    while (max_rounds-- > 0) {
        /* Build request body */
        char *body = agent->api->build_request(agent);
        if (!body) {
            fprintf(stderr, "[agent] Failed to build request\n");
            break;
        }

        if (agent->verbose) {
            fprintf(stderr, "[agent] Request body: %s\n", body);
        }

        /* Build endpoint URL */
        char *url = agent_endpoint_url(agent);

        /* Build auth header */
        char *auth_val = agent_auth_header(agent);
        char *headers[4];
        int n_headers = 0;
        headers[n_headers] = malloc(strlen(agent->api->auth_header) +
                                     strlen(auth_val) + 4);
        sprintf(headers[n_headers++], "%s: %s", agent->api->auth_header, auth_val);
        headers[n_headers++] = strdup("Content-Type: application/json");

        /* Anthropic-specific version header */
        if (agent->api->type == PROVIDER_ANTHROPIC) {
            headers[n_headers++] = strdup("anthropic-version: 2023-06-01");
        }

        /* Build HTTP request */
        http_request_t req = {
            .method       = "POST",
            .url          = url,
            .headers      = headers,
            .header_count = n_headers,
            .body         = body,
            .body_length  = strlen(body),
            .timeout_ms   = 120000,
        };

        if (agent->verbose) {
            fprintf(stderr, "[agent] POST %s\n", url);
        }

        /* Send request */
        http_response_t *resp = http_request(&req);

        /* Cleanup request data */
        free(body);
        free(url);
        free(auth_val);
        for (int i = 0; i < n_headers; i++) free(headers[i]);

        if (!resp) {
            fprintf(stderr, "[agent] HTTP request failed\n");
            break;
        }

        if (agent->verbose) {
            fprintf(stderr, "[agent] Response: status=%d, body_len=%zu\n",
                    resp->status_code, resp->body_length);
            /* Print raw response body (truncated if very long) */
            if (resp->body) {
                size_t show_len = resp->body_length;
                if (show_len > 2048) show_len = 2048;
                fprintf(stderr, "[agent] Raw body: %.*s%s\n",
                        (int)show_len, resp->body,
                        resp->body_length > 2048 ? "...(truncated)" : "");
            }
        }

        if (resp->status_code < 200 || resp->status_code >= 300) {
            fprintf(stderr, "[agent] API error (HTTP %d): %s\n",
                    resp->status_code,
                    resp->body ? resp->body : "(no body)");
            http_response_free(resp);
            break;
        }

        if (!resp->body) {
            fprintf(stderr, "[agent] Empty response body\n");
            http_response_free(resp);
            break;
        }

        /* Parse response */
        if (agent->verbose) {
            fprintf(stderr, "[agent] Parsing response (%zu bytes)...\n",
                    resp->body_length);
        }
        message_t *assistant_msg = agent_parse_response_body(
            agent->api, resp->body, agent->verbose, NULL, NULL);

        if (!assistant_msg) {
            /* Show full response body on parse failure for debugging */
            fprintf(stderr, "[agent] Failed to parse response. Raw body (%zu bytes):\n",
                    resp->body_length);
            fprintf(stderr, "[agent] --- BEGIN RAW RESPONSE ---\n");
            fprintf(stderr, "%s\n", resp->body);
            fprintf(stderr, "[agent] --- END RAW RESPONSE ---\n");
            http_response_free(resp);
            break;
        }

        /* Copy tool calls from the parsed response */
        /* (they were parsed into the message by the provider) */

        if (assistant_msg->n_tool_calls > 0) {
            if (agent->verbose) {
                fprintf(stderr, "[agent] Got %d tool call(s)\n",
                        assistant_msg->n_tool_calls);
            }

            /* Add assistant message to conversation once */
            agent_add_message(agent, assistant_msg);

            /* Execute each tool call */
            for (int i = 0; i < assistant_msg->n_tool_calls; i++) {
                tool_call_t *tc = &assistant_msg->tool_calls[i];

                if (agent->verbose) {
                    fprintf(stderr, "[agent] Executing tool: %s(%s)\n",
                            tc->name, tc->arguments);
                }

                char *error = NULL;
                char *result = tool_execute(tc->name, tc->arguments, 30000, &error);

                /* Build tool result message */
                message_t tool_msg = {
                    .role = MSG_ROLE_TOOL,
                    .content = NULL,
                    .n_tool_calls = 0,
                    .n_tool_results = 1,
                    .tool_results = calloc(1, sizeof(tool_result_t)),
                };
                tool_msg.tool_results[0].tool_call_id = strdup(tc->id);
                tool_msg.tool_results[0].content = result ? result : strdup(error ? error : "");
                tool_msg.tool_results[0].is_error = (result == NULL);

                free(error);

                /* Append tool result to conversation */
                agent_add_message(agent, &tool_msg);
            }

            message_free(assistant_msg);

            /* Loop back to send tool results to API */
            continue;
        }

        /* No tool calls — this is the final response */
        agent_add_message(agent, assistant_msg);

        /* Save the final response to return */
        final_response = message_copy(assistant_msg);
        message_free(assistant_msg);
        break;
    }

    if (max_rounds <= 0) {
        fprintf(stderr, "[agent] Max tool-use rounds exceeded\n");
    }

    if (!final_response) {
        final_response = message_create(MSG_ROLE_ASSISTANT,
            "Error: Failed to get a response from the API.");
    }

    return final_response;
}

/* ── Streaming chat ─────────────────────────────────────────────── */

/* Accumulator state for streaming */
typedef struct {
    message_t *accumulated;
    api_provider_t *api;
    char *text_buffer;
    size_t text_len;
    size_t text_cap;
} stream_ctx_t;

__attribute__((unused))
static bool sse_callback(const sse_event_t *event, void *userdata) {
    stream_ctx_t *ctx = (stream_ctx_t *)userdata;
    if (!event || !event->data) return true;

    /* Check for [DONE] signal */
    if (strcmp(event->data, "[DONE]") == 0) return true;

    /* Parse the chunk using the provider */
    message_t *delta = ctx->api->parse_chunk(event->data);
    if (!delta) return true;

    /* Accumulate text content */
    if (delta->content) {
        size_t dlen = strlen(delta->content);
        while (ctx->text_len + dlen + 1 > ctx->text_cap) {
            ctx->text_cap = ctx->text_cap ? ctx->text_cap * 2 : 4096;
            ctx->text_buffer = realloc(ctx->text_buffer, ctx->text_cap);
        }
        memcpy(ctx->text_buffer + ctx->text_len, delta->content, dlen);
        ctx->text_len += dlen;
        ctx->text_buffer[ctx->text_len] = '\0';
    }

    /* Accumulate tool calls */
    for (int i = 0; i < delta->n_tool_calls; i++) {
        tool_call_t *tc = &delta->tool_calls[i];
        message_add_tool_call(ctx->accumulated,
            tc->id, tc->name, tc->arguments);
    }

    message_free(delta);
    return true;
}

message_t *agent_chat_stream(agent_t *agent, const char *user_input,
                             void (*on_token)(const char *token, void *ctx),
                             void *ctx) {
    if (!agent || !user_input) return NULL;

    /* Add user message to conversation */
    message_t user_msg = {
        .role = MSG_ROLE_USER,
        .content = strdup(user_input),
        .n_tool_calls = 0,
        .n_tool_results = 0,
    };
    agent_add_message(agent, &user_msg);

    message_t *final_response = NULL;
    int max_rounds = 10;

    while (max_rounds-- > 0) {
        /* Note: full SSE streaming not yet implemented.
         * We use non-streaming HTTP and deliver the entire response
         * at once via the on_token callback. */

        char *body = agent->api->build_request(agent);

        if (!body) {
            fprintf(stderr, "[agent] Failed to build request\n");
            break;
        }

        char *url = agent_endpoint_url(agent);
        char *auth_val = agent_auth_header(agent);

        /* Build headers */
        char *headers[4];
        int n_headers = 0;
        headers[n_headers] = malloc(strlen(agent->api->auth_header) +
                                     strlen(auth_val) + 4);
        sprintf(headers[n_headers++], "%s: %s", agent->api->auth_header, auth_val);
        headers[n_headers++] = strdup("Content-Type: application/json");
        if (agent->api->type == PROVIDER_ANTHROPIC) {
            headers[n_headers++] = strdup("anthropic-version: 2023-06-01");
        }

        http_request_t req = {
            .method       = "POST",
            .url          = url,
            .headers      = headers,
            .header_count = n_headers,
            .body         = body,
            .body_length  = strlen(body),
            .timeout_ms   = 120000,
        };

        /* We need to read the streaming response manually for SSE parsing.
         * The current http_client doesn't have streaming support built in,
         * so we use the non-streaming endpoint as a fallback. */
        message_t *assistant_msg = NULL;

        if (agent->verbose) {
            fprintf(stderr, "[agent] POST (stream) %s\n", url);
        }

        /* For now, use non-streaming HTTP + parse the whole body.
         * Full SSE streaming over raw sockets will be added in Phase 2+. */
        http_response_t *resp = http_request(&req);

        free(body);
        free(url);
        free(auth_val);
        for (int i = 0; i < n_headers; i++) free(headers[i]);

        if (!resp || resp->status_code < 200 || resp->status_code >= 300) {
            if (resp) {
                fprintf(stderr, "[agent] API error (HTTP %d): %s\n",
                        resp->status_code,
                        resp->body ? resp->body : "(no body)");
                if (resp->body && agent->verbose) {
                    fprintf(stderr, "[agent] Raw body: %s\n", resp->body);
                }
                http_response_free(resp);
            } else {
                fprintf(stderr, "[agent] HTTP request failed\n");
            }
            break;
        }

        if (agent->verbose && resp->body) {
            fprintf(stderr, "[agent] Raw response (%zu bytes): %s\n",
                    resp->body_length, resp->body);
        }

        /* Save a copy of the body for error logging */
        char *saved_body = resp->body ? strdup(resp->body) : NULL;
        assistant_msg = agent_parse_response_body(
            agent->api, resp->body, agent->verbose, on_token, ctx);
        http_response_free(resp);

        if (!assistant_msg) {
            fprintf(stderr, "[agent] Failed to parse response\n");
            if (saved_body) {
                fprintf(stderr, "[agent] --- BEGIN RAW RESPONSE ---\n");
                fprintf(stderr, "%s\n", saved_body);
                fprintf(stderr, "[agent] --- END RAW RESPONSE ---\n");
            }
            free(saved_body);
            break;
        }
        free(saved_body);

        /* Handle tool calls */
        if (assistant_msg->n_tool_calls > 0) {
            if (agent->verbose) {
                fprintf(stderr, "[agent] Got %d tool call(s)\n",
                        assistant_msg->n_tool_calls);
            }

            /* Add assistant message once before tool results */
            agent_add_message(agent, assistant_msg);

            for (int i = 0; i < assistant_msg->n_tool_calls; i++) {
                tool_call_t *tc = &assistant_msg->tool_calls[i];

                if (agent->verbose) {
                    fprintf(stderr, "[agent] Executing tool: %s(%s)\n",
                            tc->name, tc->arguments);
                }

                char *error = NULL;
                char *result = tool_execute(tc->name, tc->arguments, 30000, &error);

                message_t tool_msg = {
                    .role = MSG_ROLE_TOOL,
                    .content = NULL,
                    .n_tool_calls = 0,
                    .n_tool_results = 1,
                    .tool_results = calloc(1, sizeof(tool_result_t)),
                };
                tool_msg.tool_results[0].tool_call_id = strdup(tc->id);
                tool_msg.tool_results[0].content = result ? result : strdup(error ? error : "");
                tool_msg.tool_results[0].is_error = (result == NULL);
                free(error);

                agent_add_message(agent, &tool_msg);
            }

            message_free(assistant_msg);
            continue;
        }

        /* Final response */
        agent_add_message(agent, assistant_msg);
        final_response = message_copy(assistant_msg);
        message_free(assistant_msg);
        break;
    }

    if (!final_response) {
        final_response = message_create(MSG_ROLE_ASSISTANT,
            "Error: Failed to get a response from the API.");
    }

    return final_response;
}
