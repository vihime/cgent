/*
 * session.c — Session persistence to ~/.cgent/sessions/<uuid>/
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
    char *file = os_path_join(dir, "session.json");
    free(dir);
    return file;
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

    json_value_t *root = json_object();
    json_object_set(root, "uuid", json_string(s->uuid));

    /* Timestamp */
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
    json_object_set(root, "created_at", json_string(ts));

    json_object_set(root, "provider", json_string(cfg->provider));
    json_object_set(root, "model", json_string(cfg->model));
    if (cfg->system_prompt)
        json_object_set(root, "system_prompt", json_string(cfg->system_prompt));

    /* Messages */
    json_value_t *msgs = json_array();
    for (int i = 0; i < s->message_count; i++) {
        message_t *m = &s->messages[i];
        json_value_t *jm = json_object();

        switch (m->role) {
        case MSG_ROLE_SYSTEM:    json_object_set(jm, "role", json_string("system")); break;
        case MSG_ROLE_USER:      json_object_set(jm, "role", json_string("user")); break;
        case MSG_ROLE_ASSISTANT:  json_object_set(jm, "role", json_string("assistant")); break;
        case MSG_ROLE_TOOL:      json_object_set(jm, "role", json_string("tool")); break;
        }
        if (m->content) json_object_set(jm, "content", json_string(m->content));

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

        json_array_append(msgs, jm);
    }
    json_object_set(root, "messages", msgs);

    char *json_str = json_stringify(root);
    json_free(root);

    char *dir = session_dir(s->uuid);
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    char *path = os_path_join(dir, "session.json");
    free(dir);

    FILE *fp = fopen(path, "w");
    if (!fp) { free(path); free(json_str); return false; }
    fputs(json_str, fp);
    fclose(fp);
    free(path);
    free(json_str);
    return true;
}

/* ── Load ────────────────────────────────────────────────────────── */

session_t *session_load(const char *uuid) {
    if (!uuid) return NULL;

    char *path = session_file(uuid);
    if (!path || !os_path_exists(path)) { free(path); return NULL; }

    FILE *fp = fopen(path, "r");
    if (!fp) { free(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > (16 * 1024 * 1024)) { fclose(fp); free(path); return NULL; }
    char *data = malloc(sz + 1);
    size_t nread = fread(data, 1, sz, fp);
    data[nread] = '\0';
    fclose(fp);
    free(path);

    json_value_t *root = json_parse(data);
    free(data);
    if (!root) return NULL;

    session_t *s = calloc(1, sizeof(session_t));

    json_value_t *v = json_object_get(root, "uuid");
    if (v && json_is_string(v)) s->uuid = strdup(json_string_value(v));

    v = json_object_get(root, "provider");
    if (v && json_is_string(v)) s->provider = strdup(json_string_value(v));

    v = json_object_get(root, "model");
    if (v && json_is_string(v)) s->model = strdup(json_string_value(v));

    v = json_object_get(root, "system_prompt");
    if (v && json_is_string(v)) s->system_prompt = strdup(json_string_value(v));

    /* Messages */
    json_value_t *msgs = json_object_get(root, "messages");
    if (msgs && json_is_array(msgs)) {
        int n = json_array_length(msgs);
        s->message_cap = n + 64;
        s->messages = calloc(s->message_cap, sizeof(message_t));

        for (int i = 0; i < n; i++) {
            json_value_t *jm = json_array_get(msgs, i);
            if (!jm || !json_is_object(jm)) continue;

            message_t *m = &s->messages[s->message_count];
            v = json_object_get(jm, "role");
            const char *role = v ? json_string_value(v) : "user";
            if (strcmp(role, "system") == 0) m->role = MSG_ROLE_SYSTEM;
            else if (strcmp(role, "assistant") == 0) m->role = MSG_ROLE_ASSISTANT;
            else if (strcmp(role, "tool") == 0) m->role = MSG_ROLE_TOOL;
            else m->role = MSG_ROLE_USER;

            v = json_object_get(jm, "content");
            if (v && json_is_string(v)) m->content = strdup(json_string_value(v));

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
    }

    json_free(root);
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
