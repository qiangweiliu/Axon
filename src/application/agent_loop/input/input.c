/* input.c — Raw-mode terminal input with UTF-8 backspace,
 *            cursor movement (←/→), and command history (↑/↓).
 *
 * Raw mode: disables echo and canonical mode so we see every keystroke.
 * Falls back to fgets when stdin is not a terminal (pipe mode).
 *
 * ANSI sequences used:
 *   \r        carriage return (go to column 0)
 *   \033[K    clear from cursor to end of line
 *   \033[<N>D move cursor left by N columns
 *   \033[<N>C move cursor right by N columns
 */

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "ansi.h"
#include "input.h"

typedef struct {
    struct termios orig_tio;
    int in_raw;
} input_ctx_t;

/* Static allocation — always available, no init order dependency */
static input_ctx_t g_storage;
static input_ctx_t *g_ctx = &g_storage;

/* "┃> " (colored) = \r + \033[1m + \033[32m + ┃(3B) + >  + \033[0m = 19 bytes.
 * Used for redrawing the prompt line. */
#define PROMPT_RAW  "\r\033[1m\033[32m┃> \033[0m"
#define PROMPT_LEN  19

/* ── History ──────────────────────────────────────────────────────────── */

#define HISTORY_MAX  50
#define LINE_MAX     4096

static char g_history[HISTORY_MAX][LINE_MAX];
static int  g_history_count = 0;

/* Saved current input while browsing history; g_history_pos=-1 = not browsing */
static char g_history_saved[LINE_MAX];
static int  g_history_saved_len = 0;
static int  g_history_pos = -1;

void history_add(const char *line)
{
    if (!line || !*line) return;
    size_t len = strlen(line);
    if (len >= LINE_MAX) len = LINE_MAX - 1;

    /* Skip duplicate of most recent entry */
    if (g_history_count > 0 &&
        strcmp(g_history[(g_history_count - 1) % HISTORY_MAX], line) == 0)
        return;

    int slot = g_history_count % HISTORY_MAX;
    memcpy(g_history[slot], line, len);
    g_history[slot][len] = '\0';
    g_history_count++;
    g_history_pos = -1;
}

/* Load a history entry into buf. offset=0 → newest, offset++ → older. */
static int history_load(int offset, char *buf, int max)
{
    if (offset < 0 || offset >= g_history_count || offset >= HISTORY_MAX)
        return -1;
    int idx = (g_history_count - 1 - offset) % HISTORY_MAX;
    size_t len = strlen(g_history[idx]);
    if (len >= (size_t)max) len = (size_t)max - 1;
    memcpy(buf, g_history[idx], len);
    buf[len] = '\0';
    return (int)len;
}

/* ── Display helpers ──────────────────────────────────────────────────── */

/* Redraw the full input line: \r + prompt + buffer + clear-to-EOL,
 * then move cursor back to the tracked byte position. */
static void redraw_line(const char *buf, int len, int cursor)
{
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, PROMPT_RAW, PROMPT_LEN);
    write(STDOUT_FILENO, buf, (size_t)len);
    write(STDOUT_FILENO, "\033[K", 3);
    if (cursor < len) {
        char move[16];
        int n = snprintf(move, sizeof(move), "\033[%dD", len - cursor);
        write(STDOUT_FILENO, move, (size_t)n);
    }
}

/* ── Raw mode ──────────────────────────────────────────────────────────── */

void raw_on(void)
{
    if (g_ctx->in_raw) return;
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_ctx->orig_tio);
    struct termios raw = g_ctx->orig_tio;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_ctx->in_raw = 1;
}

void raw_off(void)
{
    if (!g_ctx->in_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_ctx->orig_tio);
    g_ctx->in_raw = 0;
}

/* ── ESC sequence handler ──────────────────────────────────────────────── */

static void on_escape(char seq1, char seq2,
                      char *buf, int *len, int *cursor, int max)
{
    if (seq1 == '[' || seq1 == 'O') {
        /* Arrow keys send ESC [ A/B/C/D  (or ESC O A/B/C/D on some terminals) */
        int ch = seq2;  /* A=up, B=down, C=right, D=left */

        switch (ch) {
        /* ── Up ── */
        case 'A':
            if (g_history_count == 0) return;
            /* Save current input when first entering history */
            if (g_history_pos == -1) {
                g_history_saved_len = *len;
                memcpy(g_history_saved, buf, (size_t)(*len) + 1);
                g_history_pos = 0;
            } else if (g_history_pos < g_history_count - 1 &&
                       g_history_pos < HISTORY_MAX - 1) {
                g_history_pos++;
            } else {
                return; /* already at oldest */
            }
            *len = history_load(g_history_pos, buf, max);
            if (*len < 0) { *len = 0; buf[0] = '\0'; }
            *cursor = *len;
            redraw_line(buf, *len, *cursor);
            return;

        /* ── Down ── */
        case 'B':
            if (g_history_pos <= 0) {
                /* Restore saved input */
                g_history_pos = -1;
                *len = g_history_saved_len;
                memcpy(buf, g_history_saved, (size_t)(*len) + 1);
                *cursor = *len;
            } else {
                g_history_pos--;
                *len = history_load(g_history_pos, buf, max);
                if (*len < 0) { *len = 0; buf[0] = '\0'; }
                *cursor = *len;
            }
            redraw_line(buf, *len, *cursor);
            return;

        /* ── Right ── */
        case 'C':
            if (*cursor < *len) {
                (*cursor)++;
                write(STDOUT_FILENO, "\033[C", 3);
            }
            return;

        /* ── Left ── */
        case 'D':
            if (*cursor > 0) {
                /* Don't break in the middle of a multi-byte UTF-8 char */
                if ((buf[*cursor - 1] & 0xC0) == 0x80)
                    return; /* middle byte — can't position there */
                (*cursor)--;
                write(STDOUT_FILENO, "\033[D", 3);
            }
            return;
        }
    }
}

/* ── read_line_raw ─────────────────────────────────────────────────────── */

int read_line_raw(char *buf, int max)
{
    /* Fallback: if not in raw mode (pipe), use fgets */
    if (!g_ctx->in_raw) {
        if (!fgets(buf, max, stdin)) return -1;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        return (int)len;
    }

    int len = 0;
    int cursor = 0;
    buf[0] = '\0';

    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) { raw_off(); return -1; }

        /* Enter / Return */
        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            write(STDOUT_FILENO, "\r\n", 2);
            return len;
        }

        /* Backspace / Delete */
        if (c == 0x7f || c == '\b') {
            if (cursor > 0) {
                /* Scan backwards past UTF-8 continuation bytes */
                int chars = 1;
                while (chars <= cursor && (buf[cursor - chars] & 0xC0) == 0x80)
                    chars++;
                cursor -= chars;
                len -= chars;
                memmove(buf + cursor, buf + cursor + chars,
                        (size_t)(len - cursor));
                buf[len] = '\0';
                redraw_line(buf, len, cursor);
            }
            continue;
        }

        /* Ctrl-C */
        if (c == 0x03) { buf[0] = '\0'; return 0; }

        /* ESC — start of escape sequence (arrow keys, etc.) */
        if (c == 0x1b) {
            unsigned char n[2];
            if (read(STDIN_FILENO, &n[0], 1) == 1) {
                if (n[0] == '[' || n[0] == 'O') {
                    if (read(STDIN_FILENO, &n[1], 1) == 1) {
                        on_escape((char)n[0], (char)n[1],
                                  buf, &len, &cursor, max);
                    }
                }
                /* Other ESC variants are discarded */
            }
            continue;
        }

        /* Printable character (incl. UTF-8 lead+continuation bytes) */
        if (c >= 0x20 || (c & 0xC0) == 0xC0) {
            if (len < max - 4) {
                /* Insert at cursor, shift rest right */
                memmove(buf + cursor + 1, buf + cursor,
                        (size_t)(len - cursor));
                buf[cursor] = c;
                len++;
                cursor++;
                buf[len] = '\0';
                redraw_line(buf, len, cursor);
            }
        }
    }
}
