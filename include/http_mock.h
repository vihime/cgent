/*
 * http_mock.h — Mock HTTP backend for testing
 *
 * Allows injecting canned JSON responses instead of real API calls.
 * Responses are pushed onto a FIFO queue and consumed in order.
 */
#ifndef HTTP_MOCK_H
#define HTTP_MOCK_H

#include "network.h"
#include <stdbool.h>

/* ── Mock setup ─────────────────────────────────────────────────── */

/* Enable mock mode. All subsequent http_request() calls return
 * queued responses instead of making real network requests. */
void http_mock_enable(void);

/* Disable mock mode, restore real HTTP client. */
void http_mock_disable(void);

/* Check if mock mode is active. */
bool http_mock_is_enabled(void);

/* ── Response queue ─────────────────────────────────────────────── */

/* Push a response onto the mock queue.
 *   status_code: HTTP status code (200 for success)
 *   body: JSON response body (copied internally)
 * The mock returns responses in FIFO order. */
void http_mock_push(int status_code, const char *body);

/* Push a typical DeepSeek chat completion response (non-streaming).
 *   content: text content for the assistant message (NULL if tool calls)
 *   tool_call_id, tool_name, tool_args: tool call details (all NULL if no tool)
 */
void http_mock_push_chat_response(const char *content,
                                   const char *tool_call_id,
                                   const char *tool_name,
                                   const char *tool_args);

/* Internal: called by http_client.c to get a mock response. */
http_response_t *http_mock_request(const http_request_t *req);

/* Get number of remaining queued responses. */
int http_mock_queue_size(void);

/* Clear all queued responses. */
void http_mock_clear(void);

#endif /* HTTP_MOCK_H */
