/*
 * utf8_input.c — Locale-aware terminal line input
 *
 * Replaces getline() for REPL mode. Implements a minimal raw-mode
 * line editor that correctly handles multi-byte characters
 * (Chinese, Japanese, emoji, etc.) on backspace/delete.
 *
 * Auto-detects encoding from locale:
 *   UTF-8    — 1-4 byte sequences
 *   GBK      — 1-2 byte sequences (common for zh_CN)
 *   GB18030  — 1/2/4 byte sequences
 *   other    — falls back to 1-byte (ASCII safe)
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

/* ── Encoding detection ─────────────────────────────────────────── */

typedef enum {
    ENC_UTF8,
    ENC_GBK,
    ENC_ASCII
} encoding_t;

static encoding_t g_encoding = ENC_UTF8;
static bool g_encoding_detected = false;

static void detect_encoding(void) {
    if (g_encoding_detected) return;
    g_encoding_detected = true;

    setlocale(LC_ALL, "");

    const char *codeset = nl_langinfo(CODESET);
    if (!codeset) {
        g_encoding = ENC_ASCII;
        return;
    }

    char upper[64];
    size_t i;
    for (i = 0; codeset[i] && i < sizeof(upper) - 1; i++) {
        upper[i] = (codeset[i] >= 'a' && codeset[i] <= 'z')
                   ? codeset[i] - 'a' + 'A' : codeset[i];
    }
    upper[i] = '\0';

    if (strstr(upper, "UTF") || strstr(upper, "UTF-8") || strstr(upper, "UTF8")) {
        g_encoding = ENC_UTF8;
    } else if (strstr(upper, "GB") || strstr(upper, "GBK") ||
               strstr(upper, "GB18030") || strstr(upper, "GB2312") ||
               strstr(upper, "EUC-CN") || strstr(upper, "EUCCN")) {
        g_encoding = ENC_GBK;
    } else {
        g_encoding = ENC_ASCII;
    }
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
    case ENC_ASCII:
    default:
        return 1;
    }
}

/* Get byte length of the last complete character in buffer.
 * Finds the start byte of the final character and returns its
 * full byte length (1 for ASCII, 2-4 for multi-byte). */
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

    if (g_encoding == ENC_UTF8) {
        size_t start = len - 1;
        while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80) {
            start--;
        }
        int clen = encoding_char_len((unsigned char)buf[start]);
        if (clen <= 0) return 1;
        if (start + clen <= len) return clen;
        return (int)(len - start);
    }

    return 1;
}

/* ── Display width ──────────────────────────────────────────────── */

static int char_display_width(__attribute__((unused)) const char *s, int byte_len) {
    if (byte_len <= 1) return 1;
    if (g_encoding == ENC_GBK) return 2;
    if (byte_len >= 3) return 2;
    return 1;
}

/* ── Write helper ────────────────────────────────────────────────── */

static void wwrite(const void *buf, size_t count) {
    if (write(STDOUT_FILENO, buf, count) < 0) { /* ignore */ }
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

/* ── Line editor ────────────────────────────────────────────────── */

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

    while (1) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n == 0 || errno != EINTR) {
                free(buf);
                wwrite("\n", 1);
                term_raw_disable();
                return NULL;
            }
            continue;
        }

        /* Enter */
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
            return buf;
        }

        /* Ctrl-D on empty line */
        if (c == 0x04 && len == 0) {
            free(buf);
            wwrite("\n", 1);
            term_raw_disable();
            return NULL;
        }

        /* Backspace */
        if (c == 0x7f || c == '\b') {
            if (len == 0) continue;
            int char_bytes = last_char_bytes(buf, len);
            if (char_bytes <= 0 || char_bytes > (int)len) char_bytes = 1;

            int char_width = char_display_width(buf + len - char_bytes, char_bytes);
            len -= char_bytes;

            for (int i = 0; i < char_width; i++) wwrite("\b", 1);
            for (int i = 0; i < char_width; i++) wwrite(" ", 1);
            for (int i = 0; i < char_width; i++) wwrite("\b", 1);
            continue;
        }

        /* Ctrl-C */
        if (c == 0x03) {
            free(buf);
            wwrite("^C\r\n", 4);
            term_raw_disable();
            return strdup("");
        }

        /* Escape sequences */
        if (c == 0x1b) {
            unsigned char seq[8];
            ssize_t sn = read(STDIN_FILENO, seq, sizeof(seq));
            if (sn >= 2 && seq[0] == '[') {
                /* Left arrow — no-op (would need position tracking) */
                (void)seq[1];
            }
            continue;
        }

        /* Normal character */
        if (c >= 0x20) {
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
