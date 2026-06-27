/*
 * protocol.h — API provider abstraction
 *
 * Supports: Anthropic Messages API, OpenAI Chat Completions, DeepSeek Chat Completions
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "core.h"

/* ── Provider types ─────────────────────────────────────────────── */

typedef enum {
    PROVIDER_DEEPSEEK,
    PROVIDER_OPENAI,
    PROVIDER_ANTHROPIC,
    PROVIDER_COUNT
} provider_type_t;

/* ── Provider API ───────────────────────────────────────────────── */

typedef struct api_provider api_provider_t;

struct api_provider {
    provider_type_t type;
    const char *name;
    const char *default_base_url;
    const char *default_model;
    const char *auth_header;       /* e.g., "Authorization" */
    const char *auth_prefix;       /* e.g., "Bearer " */

    /* Build the JSON request body for a chat completion request.
     * Returns a malloc'd JSON string that the caller must free. */
    char *(*build_request)(const agent_t *agent);

    /* Parse a non-streaming JSON response body into a message.
     * Returns a malloc'd message_t that the caller must free. */
    message_t *(*parse_response)(const char *body);

    /* Parse a single streaming SSE data chunk into a message delta.
     * Returns a partial message_t (caller accumulates). NULL if no content. */
    message_t *(*parse_chunk)(const char *sse_data);

    /* Format tool results for the provider's API format.
     * Returns a malloc'd string (JSON array). */
    char *(*format_tool_results)(const tool_result_t *results, int count);

    /* Parse tool calls from an assistant response.
     * Returns malloc'd array, sets *count. */
    tool_call_t *(*extract_tool_calls)(const char *body, int *count);
};

/* ── Provider registry ──────────────────────────────────────────── */

/* Get provider by type */
api_provider_t *provider_get(provider_type_t type);

/* Get provider by name string ("deepseek", "openai", "anthropic") */
api_provider_t *provider_get_by_name(const char *name);

/* Initialize all providers */
void provider_init(void);

#endif /* PROTOCOL_H */
