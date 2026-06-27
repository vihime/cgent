/*
 * streaming.c — SSE streaming response accumulator
 *
 * Handles accumulation of streaming deltas from provider APIs.
 * Each provider has a different delta format, but they all accumulate
 * into a single message_t.
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

/* ── Stream accumulator ─────────────────────────────────────────── */

typedef struct {
    message_t *message;         /* Accumulated message */
    char *text_buf;             /* Accumulated text content */
    size_t text_len;
    size_t text_cap;
} stream_accum_t;

stream_accum_t *stream_accum_create(void) {
    stream_accum_t *acc = calloc(1, sizeof(stream_accum_t));
    if (!acc) return NULL;
    acc->message = calloc(1, sizeof(message_t));
    if (!acc->message) { free(acc); return NULL; }
    acc->message->role = MSG_ROLE_ASSISTANT;
    acc->text_cap = 4096;
    acc->text_buf = malloc(acc->text_cap);
    if (!acc->text_buf) { free(acc->message); free(acc); return NULL; }
    acc->text_buf[0] = '\0';
    return acc;
}

void stream_accum_add_text(stream_accum_t *acc, const char *text) {
    if (!acc || !text) return;
    size_t tlen = strlen(text);
    while (acc->text_len + tlen + 1 > acc->text_cap) {
        acc->text_cap *= 2;
        acc->text_buf = realloc(acc->text_buf, acc->text_cap);
    }
    memcpy(acc->text_buf + acc->text_len, text, tlen);
    acc->text_len += tlen;
    acc->text_buf[acc->text_len] = '\0';
}

void stream_accum_add_tool_call(stream_accum_t *acc, const char *id,
                                 const char *name, const char *args) {
    if (!acc || !acc->message) return;
    message_add_tool_call(acc->message, id, name, args);
}

message_t *stream_accum_finish(stream_accum_t *acc) {
    if (!acc) return NULL;
    if (acc->text_len > 0) {
        acc->message->content = strdup(acc->text_buf);
    }
    message_t *result = acc->message;
    free(acc->text_buf);
    free(acc);
    return result;
}

void stream_accum_free(stream_accum_t *acc) {
    if (!acc) return;
    if (acc->message) message_free(acc->message);
    free(acc->text_buf);
    free(acc);
}
