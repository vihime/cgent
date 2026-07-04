/*
 * mailbox.c — Mailbox message passing system
 *
 * Messages stored as JSON files in ~/.cgent/mailbox/
 * Each file: <uuid>.json
 */
#include "mailbox.h"
#include "json.h"
#include "platform.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Path helpers ────────────────────────────────────────────────── */

static char *mailbox_dir(void) {
    char *home = os_home_dir();
    char *dir = os_path_join(home, ".cgent/mailbox");
    free(home);
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    return dir;
}

static char *msg_path(const char *id) {
    char *dir = mailbox_dir();
    char *path = os_path_join(dir, id);
    char *full = malloc(strlen(path) + 6);
    sprintf(full, "%s.json", path);
    free(dir);
    free(path);
    return full;
}

/* ── Send ────────────────────────────────────────────────────────── */

char *mailbox_send(const char *sender, const char *recipient,
                   const char *subject, const char *body) {
    char *id = session_generate_uuid();
    if (!id) return NULL;

    json_value_t *root = json_object();
    json_object_set(root, "id", json_string(id));
    json_object_set(root, "sender", json_string(sender ? sender : "unknown"));
    json_object_set(root, "recipient", json_string(recipient ? recipient : "all"));
    json_object_set(root, "subject", json_string(subject ? subject : ""));
    json_object_set(root, "body", json_string(body ? body : ""));
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
    json_object_set(root, "timestamp", json_string(ts));
    json_object_set(root, "read", json_bool(false));

    char *json_str = json_stringify(root);
    json_free(root);

    char *path = msg_path(id);
    FILE *fp = fopen(path, "w");
    if (!fp) { free(path); free(json_str); free(id); return NULL; }
    fputs(json_str, fp);
    fclose(fp);
    free(path);
    free(json_str);
    return id;
}

/* ── Parse message from file ────────────────────────────────────── */

static mailbox_msg_t *msg_parse_file(const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 * 1024 * 1024)) { fclose(fp); return NULL; }
    char *data = malloc(sz + 1);
    size_t nr = fread(data, 1, sz, fp);
    data[nr] = '\0';
    fclose(fp);

    json_value_t *root = json_parse(data);
    free(data);
    if (!root) return NULL;

    mailbox_msg_t *msg = calloc(1, sizeof(mailbox_msg_t));
    json_value_t *v;

    v = json_object_get(root, "id");       if (v && json_is_string(v)) msg->id = strdup(json_string_value(v));
    v = json_object_get(root, "sender");   if (v && json_is_string(v)) msg->sender = strdup(json_string_value(v));
    v = json_object_get(root, "recipient");if (v && json_is_string(v)) msg->recipient = strdup(json_string_value(v));
    v = json_object_get(root, "subject");  if (v && json_is_string(v)) msg->subject = strdup(json_string_value(v));
    v = json_object_get(root, "body");     if (v && json_is_string(v)) msg->body = strdup(json_string_value(v));
    v = json_object_get(root, "timestamp");if (v && json_is_string(v)) msg->timestamp = strdup(json_string_value(v));
    v = json_object_get(root, "read");     if (v) msg->read = json_bool_value(v);

    json_free(root);
    return msg;
}

/* ── Check / List ────────────────────────────────────────────────── */

int mailbox_check(const char *recipient, mailbox_msg_t **msgs, int max) {
    int count = 0;
    char *dir = mailbox_dir();
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }

    struct dirent *entry;
    while ((entry = readdir(d)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".json") != 0) continue;

        char *path = os_path_join(dir, entry->d_name);
        mailbox_msg_t *msg = msg_parse_file(path);
        free(path);

        if (!msg) continue;

        /* Filter by recipient */
        if (recipient && recipient[0] &&
            strcmp(recipient, "all") != 0 &&
            strcmp(msg->recipient, "all") != 0 &&
            strcmp(msg->recipient, recipient) != 0) {
            mailbox_msg_free(msg);
            continue;
        }

        /* Only unread by default */
        if (!msg->read) {
            msgs[count++] = msg;
        } else {
            mailbox_msg_free(msg);
        }
    }
    closedir(d);
    free(dir);
    return count;
}

/* ── Mark read ──────────────────────────────────────────────────── */

void mailbox_mark_read(const char *msg_id) {
    char *path = msg_path(msg_id);
    mailbox_msg_t *msg = msg_parse_file(path);
    if (!msg) { free(path); return; }

    msg->read = true;

    /* Rewrite with read=true */
    json_value_t *root = json_object();
    json_object_set(root, "id", json_string(msg->id));
    json_object_set(root, "sender", json_string(msg->sender ? msg->sender : ""));
    json_object_set(root, "recipient", json_string(msg->recipient ? msg->recipient : ""));
    json_object_set(root, "subject", json_string(msg->subject ? msg->subject : ""));
    json_object_set(root, "body", json_string(msg->body ? msg->body : ""));
    json_object_set(root, "timestamp", json_string(msg->timestamp ? msg->timestamp : ""));
    json_object_set(root, "read", json_bool(true));

    char *json_str = json_stringify(root);
    json_free(root);
    mailbox_msg_free(msg);

    FILE *fp = fopen(path, "w");
    if (fp) { fputs(json_str, fp); fclose(fp); }
    free(json_str);
    free(path);
}

mailbox_msg_t *mailbox_get(const char *msg_id) {
    char *path = msg_path(msg_id);
    mailbox_msg_t *msg = msg_parse_file(path);
    free(path);
    return msg;
}

bool mailbox_delete(const char *msg_id) {
    char *path = msg_path(msg_id);
    int rc = unlink(path);
    free(path);
    return rc == 0;
}

int mailbox_unread_count(const char *recipient) {
    mailbox_msg_t *msgs[256];
    int n = mailbox_check(recipient, msgs, 256);
    for (int i = 0; i < n; i++) mailbox_msg_free(msgs[i]);
    return n;
}

int mailbox_clear(const char *recipient) {
    int count = 0;
    char *dir = mailbox_dir();
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }

    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".json") != 0) continue;

        char *path = os_path_join(dir, entry->d_name);
        mailbox_msg_t *msg = msg_parse_file(path);
        if (msg) {
            bool match = !recipient || !recipient[0] ||
                         strcmp(recipient, "all") == 0 ||
                         strcmp(msg->recipient, "all") == 0 ||
                         strcmp(msg->recipient, recipient) == 0;
            if (match) {
                unlink(path);
                count++;
            }
            mailbox_msg_free(msg);
        }
        free(path);
    }
    closedir(d);
    free(dir);
    return count;
}

void mailbox_msg_free(mailbox_msg_t *msg) {
    if (!msg) return;
    free(msg->id);
    free(msg->sender);
    free(msg->recipient);
    free(msg->subject);
    free(msg->body);
    free(msg->timestamp);
    free(msg);
}
