/* input.c — Raw-mode terminal input with UTF-8 backspace support
 *
 * Enables raw mode (no echo, no canonical), reads input byte by byte,
 * echoes printable characters, handles multi-byte UTF-8 backspace by
 * scanning backwards past continuation bytes (0x80-0xBF).
 *
 * Falls back to fgets when stdin is not a terminal (pipe mode).
 */

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "ansi.h"

typedef struct {
    struct termios orig_tio;
    int in_raw;
} input_ctx_t;

/* Static allocation — always available, no init order dependency */
static input_ctx_t g_storage;
static input_ctx_t *g_ctx = &g_storage;

/* "┃> " (colored) = \r + \033[1m + \033[32m + ┃(3B) + >  + \033[0m = 19 bytes */
#define PROMPT_RAW  "\r\033[1m\033[32m┃> \033[0m"
#define PROMPT_LEN  19

void raw_on(void)
{
    if (g_ctx->in_raw) return;
    /* Skip raw mode if stdin is not a tty (e.g. pipe input) */
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

    int pos = 0;
    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) { raw_off(); return -1; }

        if (c == '\r' || c == '\n') { buf[pos] = '\0'; return pos; }

        if (c == 0x7f || c == '\b') {
            if (pos > 0) {
                int chars = 1;
                while (chars <= pos && (buf[pos - chars] & 0xC0) == 0x80)
                    chars++;
                pos -= chars;
                buf[pos] = '\0';
                /* Redraw: prompt + buffer + clear + prompt + buffer */
                write(STDOUT_FILENO, PROMPT_RAW, PROMPT_LEN);
                write(STDOUT_FILENO, buf, (size_t)pos);
                write(STDOUT_FILENO, "\033[K", 3);
                write(STDOUT_FILENO, PROMPT_RAW, PROMPT_LEN);
                write(STDOUT_FILENO, buf, (size_t)pos);
            }
            continue;
        }

        if (c == 0x03) { buf[0] = '\0'; return 0; }

        if (c >= 0x20 || (c & 0xC0) == 0xC0) {
            if (pos < max - 4) {
                buf[pos++] = c;
                write(STDOUT_FILENO, &c, 1);
            }
        }
    }
}
