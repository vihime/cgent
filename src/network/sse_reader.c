/*
 * sse_reader.c — Server-Sent Events streaming parser
 *
 * Parses SSE streams as defined by the W3C spec:
 *   - Lines starting with "data:" are event data
 *   - Lines starting with "event:" set the event type
 *   - Lines starting with "id:" set the event ID
 *   - Empty lines dispatch the accumulated event
 */
#include "network.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct sse_parser {
    char *data_buf;       /* Accumulated data lines (joined by \n) */
    size_t data_len;
    size_t data_cap;

    char *event_type;     /* Current event type */
    char *event_id;       /* Current event ID */

    char *line_buf;       /* Partial line buffer */
    size_t line_len;
    size_t line_cap;
};

sse_parser_t *sse_parser_create(void) {
    sse_parser_t *parser = calloc(1, sizeof(sse_parser_t));
    if (!parser) return NULL;

    parser->data_cap = 16384;
    parser->data_buf = malloc(parser->data_cap);
    parser->data_len = 0;

    parser->line_cap = 8192;
    parser->line_buf = malloc(parser->line_cap);
    parser->line_len = 0;

    return parser;
}

static void dispatch_event(sse_parser_t *parser, sse_callback_t cb,
                           void *userdata) {
    if (parser->data_len == 0) return;

    sse_event_t event = {
        .event = parser->event_type ? parser->event_type : "message",
        .data  = parser->data_buf,
        .id    = parser->event_id,
        .retry = 0,
    };

    if (cb) cb(&event, userdata);

    /* Reset accumulator */
    parser->data_len = 0;
    parser->data_buf[0] = '\0';
    free(parser->event_type);
    parser->event_type = NULL;
    /* Keep event_id across events (SSE spec) */
}

static void process_line(sse_parser_t *parser, const char *line,
                         sse_callback_t cb, void *userdata) {
    /* Empty line dispatches the event */
    if (line[0] == '\0') {
        dispatch_event(parser, cb, userdata);
        return;
    }

    /* Comment line — ignore */
    if (line[0] == ':') return;

    /* Parse field:value */
    const char *colon = strchr(line, ':');
    const char *field_start = line;
    const char *value_start = colon ? colon + 1 : NULL;
    size_t field_len = colon ? (size_t)(colon - line) : strlen(line);

    /* Skip leading space in value */
    if (value_start && *value_start == ' ') value_start++;

    /* Known fields */
    if (field_len == 4 && strncmp(field_start, "data", 4) == 0) {
        /* Append data (with \n separator if not first) */
        const char *data = value_start ? value_start : "";
        size_t dlen = strlen(data);

        if (parser->data_len > 0) {
            /* Add \n separator */
            if (parser->data_len + 1 >= parser->data_cap) {
                parser->data_cap *= 2;
                parser->data_buf = realloc(parser->data_buf, parser->data_cap);
            }
            parser->data_buf[parser->data_len++] = '\n';
        }

        while (parser->data_len + dlen + 1 > parser->data_cap) {
            parser->data_cap *= 2;
            parser->data_buf = realloc(parser->data_buf, parser->data_cap);
        }
        memcpy(parser->data_buf + parser->data_len, data, dlen);
        parser->data_len += dlen;
        parser->data_buf[parser->data_len] = '\0';
    } else if (field_len == 5 && strncmp(field_start, "event", 5) == 0) {
        free(parser->event_type);
        parser->event_type = value_start ? strdup(value_start) : NULL;
    } else if (field_len == 2 && strncmp(field_start, "id", 2) == 0) {
        free(parser->event_id);
        parser->event_id = value_start ? strdup(value_start) : NULL;
    }
    /* retry field ignored for now */
}

void sse_parser_feed(sse_parser_t *parser, const char *data, size_t len,
                     sse_callback_t cb, void *userdata) {
    if (!parser || !data) return;

    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            /* Complete line */
            if (parser->line_len > 0 && parser->line_buf[parser->line_len - 1] == '\r')
                parser->line_buf[--parser->line_len] = '\0';
            parser->line_buf[parser->line_len] = '\0';
            process_line(parser, parser->line_buf, cb, userdata);
            parser->line_len = 0;
        } else {
            if (parser->line_len + 1 >= parser->line_cap) {
                parser->line_cap *= 2;
                parser->line_buf = realloc(parser->line_buf, parser->line_cap);
            }
            parser->line_buf[parser->line_len++] = c;
        }
    }
}

void sse_parser_flush(sse_parser_t *parser, sse_callback_t cb, void *userdata) {
    if (parser->line_len > 0) {
        parser->line_buf[parser->line_len] = '\0';
        process_line(parser, parser->line_buf, cb, userdata);
        parser->line_len = 0;
    }
    /* Dispatch any remaining data */
    dispatch_event(parser, cb, userdata);
}

void sse_parser_free(sse_parser_t *parser) {
    if (!parser) return;
    free(parser->data_buf);
    free(parser->event_type);
    free(parser->event_id);
    free(parser->line_buf);
    free(parser);
}

void sse_event_free(sse_event_t *event) {
    /* Events use internal parser buffers — no separate free needed */
    (void)event;
}
