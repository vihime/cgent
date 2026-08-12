/*
 * session.c — Session persistence to ~/.cgent/sessions/<uuid>/
 *
 * Sessions are saved in JSONL format: one JSON object per line.
 *   Line 1:  metadata  (uuid, created_at, provider, model, system_prompt)
 *   Line 2+: messages  (role, content, tool_calls, tool_results, raw_response)
 */
#include "session.h"
#include "json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── UUID generation ────────────────────────────────────────────── */

char *session_generate_uuid(void) {
    /* Read from /proc/sys/kernel/random/uuid (Linux) */
    FILE *fp = fopen("/proc/sys/kernel/random/uuid", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            size_t len = strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
                buf[--len] = '\0';
            return strdup(buf);
        }
        fclose(fp);
    }

    /* Fallback: generate from /dev/urandom */
    fp = fopen("/dev/urandom", "rb");
    if (!fp) {
        /* Last resort: timestamp + pid */
        char buf[64];
        snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%04x-%012lx",
                 (unsigned long)time(NULL), rand() & 0xFFFF, rand() & 0xFFFF,
                 (rand() & 0x0FFF) | 0x4000,
                 ((unsigned long)rand() << 32) | (unsigned long)rand());
        return strdup(buf);
    }

    unsigned char r[16];
    if (fread(r, 1, 16, fp) < 16) { /* fall through */ }
    fclose(fp);
    r[6] = (r[6] & 0x0f) | 0x40;  /* Version 4 */
    r[8] = (r[8] & 0x3f) | 0x80;  /* Variant */

    char buf[64];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return strdup(buf);
}

/* ── Path helpers ────────────────────────────────────────────────── */

char *session_dir(const char *uuid) {
    char *home = os_home_dir();
    char *sessions = os_path_join(home, ".cgent/sessions");
    char *dir = os_path_join(sessions, uuid);
    free(home);
    free(sessions);
    return dir;
}

static char *session_file(const char *uuid) {
    char *dir = session_dir(uuid);
    char *file = os_path_join(dir, "session.jsonl");
    free(dir);
    return file;
}

/* ── Serialize one message to a JSON object ─────────────────────── */

static json_value_t *message_to_json(const message_t *m) {
    json_value_t *jm = json_object();

    switch (m->role) {
    case MSG_ROLE_SYSTEM:    json_object_set(jm, "role", json_string("system")); break;
    case MSG_ROLE_USER:      json_object_set(jm, "role", json_string("user")); break;
    case MSG_ROLE_ASSISTANT:  json_object_set(jm, "role", json_string("assistant")); break;
    case MSG_ROLE_TOOL:      json_object_set(jm, "role", json_string("tool")); break;
    }
    if (m->content) json_object_set(jm, "content", json_string(m->content));

    /* Reasoning content (DeepSeek R1, OpenAI o1 thinking) */
    if (m->reasoning_content && m->reasoning_content[0])
        json_object_set(jm, "reasoning_content", json_string(m->reasoning_content));

    /* Raw API response (for assistant messages) */
    if (m->raw_response && m->raw_response[0])
        json_object_set(jm, "raw_response", json_string(m->raw_response));

    /* Tool calls */
    if (m->n_tool_calls > 0) {
        json_value_t *tcs = json_array();
        for (int j = 0; j < m->n_tool_calls; j++) {
            json_value_t *tc = json_object();
            json_object_set(tc, "id", json_string(m->tool_calls[j].id));
            json_object_set(tc, "name", json_string(m->tool_calls[j].name));
            json_object_set(tc, "arguments", json_string(m->tool_calls[j].arguments));
            json_array_append(tcs, tc);
        }
        json_object_set(jm, "tool_calls", tcs);
    }

    /* Tool results */
    if (m->n_tool_results > 0) {
        json_value_t *trs = json_array();
        for (int j = 0; j < m->n_tool_results; j++) {
            json_value_t *tr = json_object();
            json_object_set(tr, "tool_call_id", json_string(m->tool_results[j].tool_call_id));
            json_object_set(tr, "content", json_string(m->tool_results[j].content));
            json_object_set(tr, "is_error", json_bool(m->tool_results[j].is_error));
            json_array_append(trs, tr);
        }
        json_object_set(jm, "tool_results", trs);
    }

    return jm;
}

/* ── Save ────────────────────────────────────────────────────────── */

bool session_create(session_t *s, const cgent_config_t *cfg) {
    if (!s || !s->uuid) return false;
    char *dir = session_dir(s->uuid);
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    free(dir);
    return session_save(s, cfg);
}

bool session_save(session_t *s, const cgent_config_t *cfg) {
    if (!s || !s->uuid) return false;

    char *dir = session_dir(s->uuid);
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    char *path = os_path_join(dir, "session.jsonl");
    free(dir);

    FILE *fp = fopen(path, "w");
    if (!fp) { free(path); return false; }

    /* ── Line 1: Metadata ─────────────────────────────────────── */
    json_value_t *meta = json_object();
    json_object_set(meta, "type", json_string("metadata"));
    json_object_set(meta, "uuid", json_string(s->uuid));

    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
    json_object_set(meta, "created_at", json_string(ts));

    json_object_set(meta, "provider", json_string(cfg->provider));
    json_object_set(meta, "model", json_string(cfg->model));
    if (cfg->system_prompt)
        json_object_set(meta, "system_prompt", json_string(cfg->system_prompt));
    json_object_set(meta, "prompt_tokens", json_number((double)s->prompt_tokens));
    json_object_set(meta, "completion_tokens", json_number((double)s->completion_tokens));
    json_object_set(meta, "request_count", json_number(s->request_count));
    json_object_set(meta, "retry_count", json_number(s->retry_count));

    char *meta_str = json_stringify(meta);
    json_free(meta);
    fputs(meta_str, fp);
    fputc('\n', fp);
    free(meta_str);

    /* ── Lines 2+: Messages ───────────────────────────────────── */
    for (int i = 0; i < s->message_count; i++) {
        json_value_t *jm = message_to_json(&s->messages[i]);
        char *msg_str = json_stringify(jm);
        json_free(jm);
        fputs(msg_str, fp);
        fputc('\n', fp);
        free(msg_str);
    }

    fclose(fp);
    free(path);
    return true;
}

/* ── Load ────────────────────────────────────────────────────────── */

/* Parse one message from a JSON object, appending to session */
static void session_parse_message(session_t *s, json_value_t *jm) {
    /* Skip metadata line (has "type" field) */
    json_value_t *type = json_object_get(jm, "type");
    if (type && json_is_string(type)) {
        /* This is the metadata line — restore session-level fields */
        json_value_t *v = json_object_get(jm, "uuid");
        if (v && json_is_string(v)) {
            free(s->uuid);
            s->uuid = strdup(json_string_value(v));
        }
        v = json_object_get(jm, "provider");
        if (v && json_is_string(v)) {
            free(s->provider);
            s->provider = strdup(json_string_value(v));
        }
        v = json_object_get(jm, "model");
        if (v && json_is_string(v)) {
            free(s->model);
            s->model = strdup(json_string_value(v));
        }
        v = json_object_get(jm, "system_prompt");
        if (v && json_is_string(v)) {
            free(s->system_prompt);
            s->system_prompt = strdup(json_string_value(v));
        }
        v = json_object_get(jm, "created_at");
        if (v && json_is_string(v)) {
            free(s->created_at);
            s->created_at = strdup(json_string_value(v));
        }
        v = json_object_get(jm, "prompt_tokens");
        if (v && json_is_number(v)) s->prompt_tokens = (long long)json_number_value(v);
        v = json_object_get(jm, "completion_tokens");
        if (v && json_is_number(v)) s->completion_tokens = (long long)json_number_value(v);
        v = json_object_get(jm, "request_count");
        if (v && json_is_number(v)) s->request_count = (int)json_number_value(v);
        v = json_object_get(jm, "retry_count");
        if (v && json_is_number(v)) s->retry_count = (int)json_number_value(v);
        return;
    }

    /* Regular message line */
    if (s->message_count >= s->message_cap) {
        s->message_cap = s->message_cap ? s->message_cap * 2 : 64;
        s->messages = realloc(s->messages, s->message_cap * sizeof(message_t));
    }

    message_t *m = &s->messages[s->message_count];
    memset(m, 0, sizeof(message_t));

    json_value_t *v = json_object_get(jm, "role");
    const char *role = v ? json_string_value(v) : "user";
    if (strcmp(role, "system") == 0) m->role = MSG_ROLE_SYSTEM;
    else if (strcmp(role, "assistant") == 0) m->role = MSG_ROLE_ASSISTANT;
    else if (strcmp(role, "tool") == 0) m->role = MSG_ROLE_TOOL;
    else m->role = MSG_ROLE_USER;

    v = json_object_get(jm, "content");
    if (v && json_is_string(v)) m->content = strdup(json_string_value(v));

    /* Reasoning content */
    v = json_object_get(jm, "reasoning_content");
    if (v && json_is_string(v)) m->reasoning_content = strdup(json_string_value(v));

    /* Raw API response */
    v = json_object_get(jm, "raw_response");
    if (v && json_is_string(v)) m->raw_response = strdup(json_string_value(v));

    /* Tool calls */
    json_value_t *tcs = json_object_get(jm, "tool_calls");
    if (tcs && json_is_array(tcs)) {
        int tn = json_array_length(tcs);
        for (int j = 0; j < tn; j++) {
            json_value_t *tc = json_array_get(tcs, j);
            json_value_t *tid = json_object_get(tc, "id");
            json_value_t *tnm = json_object_get(tc, "name");
            json_value_t *targs = json_object_get(tc, "arguments");
            message_add_tool_call(m,
                tid ? json_string_value(tid) : "",
                tnm ? json_string_value(tnm) : "",
                targs ? json_string_value(targs) : "{}");
        }
    }

    /* Tool results */
    json_value_t *trs = json_object_get(jm, "tool_results");
    if (trs && json_is_array(trs)) {
        int tn = json_array_length(trs);
        for (int j = 0; j < tn; j++) {
            json_value_t *tr = json_array_get(trs, j);
            json_value_t *tid = json_object_get(tr, "tool_call_id");
            json_value_t *tcnt = json_object_get(tr, "content");
            json_value_t *terr = json_object_get(tr, "is_error");
            if (tid) {
                message_add_tool_result(m,
                    json_string_value(tid),
                    tcnt ? json_string_value(tcnt) : "",
                    terr ? json_bool_value(terr) : false);
            }
        }
    }

    s->message_count++;
}

session_t *session_load(const char *uuid) {
    if (!uuid) return NULL;

    char *path = session_file(uuid);
    if (!path || !os_path_exists(path)) { free(path); return NULL; }

    FILE *fp = fopen(path, "r");
    if (!fp) { free(path); return NULL; }

    session_t *s = calloc(1, sizeof(session_t));
    if (!s) { fclose(fp); free(path); return NULL; }
    s->message_cap = 64;
    s->messages = calloc(s->message_cap, sizeof(message_t));

    /* Read line by line */
    char *line = NULL;
    size_t line_cap = 0;

    while (1) {
        ssize_t nread = getline(&line, &line_cap, fp);
        if (nread < 0) break; /* EOF or error */

        /* Strip trailing newline */
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
            nread--;
        }
        if (nread > 0 && line[nread - 1] == '\r') {
            line[nread - 1] = '\0';
        }

        /* Skip empty lines */
        if (nread == 0) continue;

        json_value_t *jm = json_parse(line);
        if (!jm) continue; /* Skip unparseable lines */

        session_parse_message(s, jm);
        json_free(jm);
    }

    free(line);
    fclose(fp);
    free(path);

    /* If no uuid was restored from metadata, use the parameter */
    if (!s->uuid) s->uuid = strdup(uuid);

    return s;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

void session_add_message(session_t *s, const message_t *msg) {
    if (!s || !msg) return;
    if (s->message_count >= s->message_cap) {
        s->message_cap = s->message_cap ? s->message_cap * 2 : 64;
        s->messages = realloc(s->messages, s->message_cap * sizeof(message_t));
    }
    message_t *copy = message_copy(msg);
    s->messages[s->message_count++] = *copy;
    free(copy);
}

void session_free(session_t *s) {
    if (!s) return;
    free(s->uuid);
    free(s->created_at);
    free(s->provider);
    free(s->model);
    free(s->system_prompt);
    for (int i = 0; i < s->message_count; i++)
        message_clear(&s->messages[i]);
    free(s->messages);
    free(s);
}
