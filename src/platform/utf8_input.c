/*
 * utf8_input.c — Locale-aware terminal line input
 *
 * Raw-mode line editor with:
 *   - UTF-8 / GBK / GB18030 multi-byte character handling
 *   - Backspace deletes full characters (by display width)
 *   - History navigation (up/down arrow keys)
 *
 * Zero external dependencies — uses POSIX termios + langinfo.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <locale.h>
#include <langinfo.h>
#include <stdbool.h>

/* ── History ────────────────────────────────────────────────────── */

#define HISTORY_MAX 256

static char *g_history[HISTORY_MAX];
static int   g_history_count = 0;
static int   g_history_pos = -1;    /* -1 = editing new line */
static char *g_saved_line = NULL;   /* Saved current line during navigation */

/* Add a line to history (skip duplicates, skip empty) */
static void history_add(const char *line) {
    if (!line || !line[0]) return;
    /* Skip if same as last entry */
    if (g_history_count > 0 && g_saved_line) {
        free(g_saved_line);
        g_saved_line = NULL;
    }
    if (g_history_count > 0 && strcmp(g_history[g_history_count - 1], line) == 0)
        return;
    if (g_history_count >= HISTORY_MAX) {
        free(g_history[0]);
        memmove(g_history, g_history + 1, (HISTORY_MAX - 1) * sizeof(char *));
        g_history_count--;
    }
    g_history[g_history_count++] = strdup(line);
}

/* Navigate to a specific history entry. Returns the entry text. */
static const char *history_get(int pos) {
    if (pos < 0 || pos >= g_history_count) return "";
    return g_history[pos];
}

/* ── Encoding detection ─────────────────────────────────────────── */

typedef enum { ENC_UTF8, ENC_GBK, ENC_ASCII } encoding_t;

static encoding_t g_encoding = ENC_UTF8;
static bool g_encoding_detected = false;

static void detect_encoding(void) {
    if (g_encoding_detected) return;
    g_encoding_detected = true;
    setlocale(LC_ALL, "");
    const char *codeset = nl_langinfo(CODESET);
    if (!codeset) { g_encoding = ENC_ASCII; return; }
    char upper[64];
    size_t i;
    for (i = 0; codeset[i] && i < sizeof(upper) - 1; i++)
        upper[i] = (codeset[i] >= 'a' && codeset[i] <= 'z')
                   ? codeset[i] - 'a' + 'A' : codeset[i];
    upper[i] = '\0';
    if (strstr(upper, "UTF")) g_encoding = ENC_UTF8;
    else if (strstr(upper, "GB")) g_encoding = ENC_GBK;
    else g_encoding = ENC_ASCII;
}

/* ── Character length by encoding ────────────────────────────────── */

static int encoding_char_len(unsigned char c) {
    switch (g_encoding) {
    case ENC_UTF8:
        if (c < 0x80) return 1;
        if (c < 0xC0) return 0;
        if (c < 0xE0) return 2;
        if (c < 0xF0) return 3;
        if (c < 0xF8) return 4;
        return 0;
    case ENC_GBK:
        if (c < 0x80) return 1;
        if (c >= 0x81 && c <= 0xFE) return 2;
        return 1;
    default: return 1;
    }
}

static int last_char_bytes(const char *buf, size_t len) {
    if (len == 0) return 0;
    unsigned char last = (unsigned char)buf[len - 1];
    if (last < 0x80) return 1;
    if (g_encoding == ENC_GBK) {
        if (last >= 0x81 && last <= 0xFE) return 1;
        if (len >= 2) {
            unsigned char prev = (unsigned char)buf[len - 2];
            if (prev >= 0x81 && prev <= 0xFE) return 2;
        }
        return 1;
    }
    /* UTF-8 */
    size_t start = len - 1;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80) start--;
    int clen = encoding_char_len((unsigned char)buf[start]);
    if (clen <= 0) return 1;
    if (start + clen <= len) return clen;
    return (int)(len - start);
}

static int char_display_width(__attribute__((unused)) const char *s, int byte_len) {
    if (byte_len <= 1) return 1;
    if (g_encoding == ENC_GBK) return 2;
    if (byte_len >= 3) return 2;
    return 1;
}

/* ── Terminal & screen helpers ──────────────────────────────────── */

static void wwrite(const void *buf, size_t count) {
    if (write(STDOUT_FILENO, buf, count) < 0) { /* ignore */ }
}

/* Clear current line from cursor to end, then redraw prompt + text */
static void redraw_line(const char *prompt, const char *text) {
    wwrite("\r", 1);                    /* Go to column 0 */
    wwrite("\033[0K", 4);              /* Erase to end of line */
    if (prompt) wwrite(prompt, strlen(prompt));
    if (text) wwrite(text, strlen(text));
}

/* ── Terminal raw mode ──────────────────────────────────────────── */

static struct termios g_orig_termios;
static bool g_term_raw = false;

static void term_raw_enable(void) {
    if (g_term_raw) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_term_raw = true;
}

static void term_raw_disable(void) {
    if (!g_term_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_term_raw = false;
}

/* ── Line editor with history ────────────────────────────────────── */

#define LINE_INIT_CAP 256

char *utf8_readline(const char *prompt) {
    detect_encoding();

    if (!isatty(STDIN_FILENO)) {
        if (prompt) { printf("%s", prompt); fflush(stdout); }
        char *line = NULL;
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) { free(line); return NULL; }
        if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
        if (n > 1 && line[n-2] == '\r') line[n-2] = '\0';
        return line;
    }

    term_raw_enable();

    if (prompt) wwrite(prompt, strlen(prompt));

    char *buf = malloc(LINE_INIT_CAP);
    if (!buf) { term_raw_disable(); return NULL; }
    size_t cap = LINE_INIT_CAP;
    size_t len = 0;

    /* Reset history navigation state */
    g_history_pos = -1;
    free(g_saved_line);
    g_saved_line = NULL;

    while (1) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n == 0 || errno != EINTR) {
                free(buf);
                free(g_saved_line); g_saved_line = NULL;
                wwrite("\n", 1);
                term_raw_disable();
                return NULL;
            }
            continue;
        }

        /* ── Enter ──────────────────────────────────────────── */
        if (c == '\r' || c == '\n') {
            if (c == '\r') {
                struct termios tmp;
                tcgetattr(STDIN_FILENO, &tmp);
                tmp.c_cc[VMIN] = 0;
                tmp.c_cc[VTIME] = 1;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &tmp);
                unsigned char peek;
                if (read(STDIN_FILENO, &peek, 1) < 0) { /* ignore */ }
                term_raw_enable();
            }
            buf[len] = '\0';
            wwrite("\r\n", 2);
            term_raw_disable();

            /* Add to history */
            history_add(buf);

            free(g_saved_line); g_saved_line = NULL;
            return buf;
        }

        /* ── Ctrl-D on empty line ──────────────────────────── */
        if (c == 0x04 && len == 0) {
            free(buf);
            free(g_saved_line); g_saved_line = NULL;
            wwrite("\n", 1);
            term_raw_disable();
            return NULL;
        }

        /* ── Backspace ──────────────────────────────────────── */
        if (c == 0x7f || c == '\b') {
            if (len == 0) continue;
            int char_bytes = last_char_bytes(buf, len);
            if (char_bytes <= 0 || char_bytes > (int)len) char_bytes = 1;
            int char_width = char_display_width(buf + len - char_bytes, char_bytes);
            len -= char_bytes;
            for (int i = 0; i < char_width; i++) wwrite("\b", 1);
            for (int i = 0; i < char_width; i++) wwrite(" ", 1);
            for (int i = 0; i < char_width; i++) wwrite("\b", 1);
            g_history_pos = -1;  /* Editing breaks history navigation */
            continue;
        }

        /* ── Ctrl-C ─────────────────────────────────────────── */
        if (c == 0x03) {
            free(buf);
            free(g_saved_line); g_saved_line = NULL;
            wwrite("^C\r\n", 4);
            term_raw_disable();
            return strdup("");
        }

        /* ── Escape sequences (arrow keys) ──────────────────── */
        if (c == 0x1b) {
            unsigned char seq[8];
            ssize_t sn = read(STDIN_FILENO, seq, sizeof(seq));
            if (sn < 2) continue;

            if (seq[0] == '[') {
                if (seq[1] == 'A') {
                    /* ── Up arrow ────────────────────────── */
                    if (g_history_count == 0) continue;

                    /* Save current line if entering history */
                    if (g_history_pos < 0) {
                        free(g_saved_line);
                        buf[len] = '\0';
                        g_saved_line = strdup(buf);
                        g_history_pos = g_history_count - 1;
                    } else if (g_history_pos > 0) {
                        g_history_pos--;
                    } else {
                        continue; /* Already at oldest */
                    }

                    /* Load history entry into buffer */
                    const char *h = history_get(g_history_pos);
                    size_t hlen = strlen(h);
                    while (hlen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf, h, hlen);
                    len = hlen;

                    /* Redraw the line */
                    redraw_line(prompt, buf);
                    continue;
                } else if (seq[1] == 'B') {
                    /* ── Down arrow ──────────────────────── */
                    if (g_history_pos < 0) continue; /* Not navigating */

                    if (g_history_pos < g_history_count - 1) {
                        /* Go to newer entry */
                        g_history_pos++;
                        const char *h = history_get(g_history_pos);
                        size_t hlen = strlen(h);
                        while (hlen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                        memcpy(buf, h, hlen);
                        len = hlen;
                    } else {
                        /* Past the newest — restore saved line or blank */
                        g_history_pos = -1;
                        if (g_saved_line) {
                            size_t slen = strlen(g_saved_line);
                            while (slen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                            memcpy(buf, g_saved_line, slen);
                            len = slen;
                            free(g_saved_line); g_saved_line = NULL;
                        } else {
                            len = 0;
                        }
                    }

                    /* Redraw the line */
                    buf[len] = '\0';
                    redraw_line(prompt, buf);
                    continue;
                }
                /* Other escape sequences ignored */
            }
            continue;
        }

        /* ── Normal character ──────────────────────────────── */
        if (c >= 0x20) {
            /* Any typing breaks history navigation */
            if (g_history_pos >= 0) {
                g_history_pos = -1;
                free(g_saved_line); g_saved_line = NULL;
            }

            if (len + 5 >= cap) {
                cap *= 2;
                char *newbuf = realloc(buf, cap);
                if (!newbuf) { free(buf); term_raw_disable(); return NULL; }
                buf = newbuf;
            }
            buf[len++] = c;
            wwrite(&c, 1);
        }
    }
}
