/*
 * mailbox.h — Inter-agent mailbox communication
 *
 * Messages stored in ~/.cgent/mailbox/<id>.json
 * Supports subagent ↔ parent and user ↔ agent messaging.
 */
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stddef.h>

/* ── Message ──────────────────────────────────────────────────── */

typedef struct {
    char *id;           /* Unique message ID */
    char *sender;       /* Who sent it */
    char *recipient;    /* Intended recipient (or "all") */
    char *subject;      /* Message subject */
    char *body;         /* Message body */
    char *timestamp;    /* Unix timestamp string */
    bool  read;         /* Has been read? */
} mailbox_msg_t;

/* ── API ───────────────────────────────────────────────────────── */

/* Send a message to the mailbox. Returns message ID (malloc'd). */
char *mailbox_send(const char *sender, const char *recipient,
                   const char *subject, const char *body);

/* List unread messages for a recipient. Returns count.
 * Caller passes a pre-allocated array of mailbox_msg_t pointers. */
int  mailbox_check(const char *recipient, mailbox_msg_t **msgs, int max);

/* Mark a message as read */
void mailbox_mark_read(const char *msg_id);

/* Get a message by ID. Returns malloc'd message or NULL. */
mailbox_msg_t *mailbox_get(const char *msg_id);

/* Delete a message by ID */
bool mailbox_delete(const char *msg_id);

/* Count unread messages for recipient */
int  mailbox_unread_count(const char *recipient);

/* Clear all messages for recipient */
int  mailbox_clear(const char *recipient);

/* Free a message */
void mailbox_msg_free(mailbox_msg_t *msg);

#endif /* MAILBOX_H */
