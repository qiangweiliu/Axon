/* agent_private.h — Internal shared types for agent_loop/ */

#ifndef AGENT_PRIVATE_H
#define AGENT_PRIVATE_H

#include "memfile.h"

/* ANSI colors (also defined in ansi.h) */
#define RST  "\033[0m"
#define BLD  "\033[1m"
#define DIM  "\033[2m"
#define GRN  "\033[32m"
#define CYN  "\033[36m"
#define YLW  "\033[33m"
#define GRY  "\033[90m"
#define BLU  "\033[34m"
#define RED  "\033[31m"

#define PROMPT_PATH_DEFAULT "prompt.txt"
#define PROMPT_MAX          1024
#define TICK_COOLDOWN       5
#define LINE_WIDTH          60

#define MEMFILE_MEMORY_LIMIT  8000
#define MEMFILE_USER_LIMIT    4000

typedef struct {
    memfile_t mem;
    memfile_t user;
    char prompt_path[256];
    int  tick_count;
    int  session_tokens;
    int  first_token;
    int  saw_reasoning;
} agent_loop_ctx_t;

extern agent_loop_ctx_t *g_ctx;
extern volatile int g_spinner_on;

/* Handler declarations */
void handle_echo(const char *msg, char *out, size_t out_len);
void handle_remember(const char *content, char *out, size_t out_len);
int  handle_recall(const char *query, char *out, size_t out_len);
void handle_note(const char *text, char *out, size_t out_len);
void handle_profile(const char *text, char *out, size_t out_len);
void handle_replace(const char *args, char *out, size_t out_len);
void handle_forget(const char *raw, char *out, size_t out_len);
void handle_notes(char *out, size_t out_len);
int  handle_ask(const char *question, char *out, size_t out_len);

#endif /* AGENT_PRIVATE_H */
