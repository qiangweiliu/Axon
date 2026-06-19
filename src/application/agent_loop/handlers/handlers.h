/* handlers.h — Command handler declarations */
#ifndef AGENT_HANDLERS_H
#define AGENT_HANDLERS_H

#include <stddef.h>

void handle_echo(const char *msg, char *out, size_t out_len);
void handle_remember(const char *content, char *out, size_t out_len);
int  handle_recall(const char *query, char *out, size_t out_len);
void handle_note(const char *text, char *out, size_t out_len);
void handle_profile(const char *text, char *out, size_t out_len);
void handle_replace(const char *args, char *out, size_t out_len);
void handle_forget(const char *raw, char *out, size_t out_len);
void handle_notes(char *out, size_t out_len);

#endif
