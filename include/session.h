/*
 * session.h — Session persistence
 *
 * Sessions are saved to ~/.cgent/sessions/<uuid>/session.jsonl
 * JSONL format: one JSON object per line (metadata + messages).
 */
#ifndef SESSION_H
#define SESSION_H

#include "core.h"
#include "config.h"

/* ── Session structure ──────────────────────────────────────────── */

typedef struct {
    char *uuid;
    char *created_at;
    char *provider;
    char *model;
    char *system_prompt;
    message_t *messages;
    int message_count;
    int message_cap;
    /* Cumulative usage / observability (persisted in session metadata) */
    long long prompt_tokens;
    long long completion_tokens;
    int request_count;
    int retry_count;
} session_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Generate a new UUID (v4 format) */
char *session_generate_uuid(void);

/* Create a new session directory and save initial state */
bool session_create(session_t *s, const cgent_config_t *cfg);

/* Save session state (messages, config) to disk */
bool session_save(session_t *s, const cgent_config_t *cfg);

/* Load a session from ~/.cgent/sessions/<uuid>/ */
session_t *session_load(const char *uuid);

/* Get the session directory path (~/.cgent/sessions/<uuid>/) */
char *session_dir(const char *uuid);

/* Append a message to session */
void session_add_message(session_t *s, const message_t *msg);

/* Free a session */
void session_free(session_t *s);

#endif /* SESSION_H */
