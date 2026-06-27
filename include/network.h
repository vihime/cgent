/*
 * network.h — HTTP/HTTPS client, SSE reader, WebSocket
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>

/* ── HTTP ───────────────────────────────────────────────────────── */

typedef struct {
    int status_code;
    char **headers;
    int header_count;
    char *body;
    size_t body_length;
} http_response_t;

typedef struct {
    char *method;
    char *url;
    char **headers;
    int header_count;
    char *body;
    size_t body_length;
    long timeout_ms;
} http_request_t;

/* Initialize HTTP client (global OpenSSL init) */
int  http_init(void);
void http_cleanup(void);

/* Synchronous HTTP request */
http_response_t *http_request(const http_request_t *req);
void             http_response_free(http_response_t *resp);
void             http_request_free(http_request_t *req);

/* ── SSE (Server-Sent Events) ───────────────────────────────────── */

typedef struct {
    char *event;    /* event type, or "message" if unspecified */
    char *data;     /* event data */
    char *id;       /* event id (optional) */
    int   retry;    /* retry interval (optional) */
} sse_event_t;

/* Callback for SSE events. Return false to stop streaming. */
typedef bool (*sse_callback_t)(const sse_event_t *event, void *userdata);

/* SSE parser state. Handles partial lines and buffering. */
typedef struct sse_parser sse_parser_t;

sse_parser_t *sse_parser_create(void);
void          sse_parser_feed(sse_parser_t *parser, const char *data, size_t len,
                              sse_callback_t cb, void *userdata);
void          sse_parser_flush(sse_parser_t *parser, sse_callback_t cb, void *userdata);
void          sse_parser_free(sse_parser_t *parser);

void          sse_event_free(sse_event_t *event);

/* ── URL parsing ────────────────────────────────────────────────── */

typedef struct {
    char *scheme;
    char *host;
    int   port;
    char *path;
} parsed_url_t;

parsed_url_t *url_parse(const char *url);
void          url_free(parsed_url_t *url);

#endif /* NETWORK_H */
