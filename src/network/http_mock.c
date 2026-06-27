/*
 * http_mock.c — Mock HTTP backend for testing
 *
 * Intercepts http_request() calls and returns canned responses.
 */
#include "http_mock.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Mock state ─────────────────────────────────────────────────── */

static bool g_mock_enabled = false;

/* Queued response */
typedef struct mock_response {
    int status_code;
    char *body;
    size_t body_length;
    struct mock_response *next;
} mock_response_t;

static mock_response_t *g_mock_head = NULL;
static mock_response_t *g_mock_tail = NULL;
static int g_mock_count = 0;

/* ── Mock enable/disable ────────────────────────────────────────── */

void http_mock_enable(void)  { g_mock_enabled = true; }
void http_mock_disable(void) { g_mock_enabled = false; }
bool http_mock_is_enabled(void) { return g_mock_enabled; }

/* ── Queue management ───────────────────────────────────────────── */

void http_mock_push(int status_code, const char *body) {
    mock_response_t *mr = calloc(1, sizeof(mock_response_t));
    mr->status_code = status_code;
    if (body) {
        mr->body = strdup(body);
        mr->body_length = strlen(body);
    }
    mr->next = NULL;

    if (g_mock_tail) {
        g_mock_tail->next = mr;
    } else {
        g_mock_head = mr;
    }
    g_mock_tail = mr;
    g_mock_count++;
}

void http_mock_push_chat_response(const char *content,
                                   const char *tool_call_id,
                                   const char *tool_name,
                                   const char *tool_args) {
    json_value_t *root = json_object();
    json_value_t *choices = json_array();
    json_value_t *choice = json_object();
    json_value_t *message = json_object();

    json_object_set(message, "role", json_string("assistant"));

    if (content && content[0]) {
        json_object_set(message, "content", json_string(content));
    } else {
        json_object_set(message, "content", json_null());
    }

    if (tool_call_id && tool_name && tool_args) {
        json_value_t *tcs = json_array();
        json_value_t *tc = json_object();
        json_object_set(tc, "id", json_string(tool_call_id));
        json_object_set(tc, "type", json_string("function"));
        json_value_t *func = json_object();
        json_object_set(func, "name", json_string(tool_name));
        json_object_set(func, "arguments", json_string(tool_args));
        json_object_set(tc, "function", func);
        json_array_append(tcs, tc);
        json_object_set(message, "tool_calls", tcs);
    }

    json_object_set(choice, "index", json_number(0));
    json_object_set(choice, "message", message);
    json_object_set(choice, "finish_reason", json_string("stop"));
    json_array_append(choices, choice);

    json_object_set(root, "id", json_string("mock-chat-completion"));
    json_object_set(root, "object", json_string("chat.completion"));
    json_object_set(root, "created", json_number(1234567890));
    json_object_set(root, "model", json_string("deepseek-chat"));
    json_object_set(root, "choices", choices);

    char *body = json_stringify(root);
    http_mock_push(200, body);
    free(body);
    json_free(root);
}

int http_mock_queue_size(void) {
    return g_mock_count;
}

void http_mock_clear(void) {
    mock_response_t *mr = g_mock_head;
    while (mr) {
        mock_response_t *next = mr->next;
        free(mr->body);
        free(mr);
        mr = next;
    }
    g_mock_head = NULL;
    g_mock_tail = NULL;
    g_mock_count = 0;
}

/* ── Mock HTTP request ──────────────────────────────────────────── */

/* Called by http_client.c when mock mode is active */
http_response_t *http_mock_request(const http_request_t *req) {
    (void)req; /* Request details are ignored — we return queued responses */

    if (g_mock_count == 0) {
        /* No more responses queued — return a generic error */
        http_response_t *resp = calloc(1, sizeof(http_response_t));
        resp->status_code = 500;
        resp->body = strdup("{\"error\":\"Mock: no responses queued\"}");
        resp->body_length = strlen(resp->body);
        return resp;
    }

    /* Pop from front of queue */
    mock_response_t *mr = g_mock_head;
    g_mock_head = mr->next;
    if (!g_mock_head) g_mock_tail = NULL;
    g_mock_count--;

    /* Build response */
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    resp->status_code = mr->status_code;
    resp->body = mr->body;  /* Transfer ownership */
    resp->body_length = mr->body_length;
    resp->headers = NULL;
    resp->header_count = 0;

    free(mr);
    return resp;
}
