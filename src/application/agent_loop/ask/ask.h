/* ask.h — LLM interaction declarations */
#ifndef AGENT_ASK_H
#define AGENT_ASK_H

#include <stddef.h>
#include <stdint.h>

int handle_ask(const char *question, char *out, size_t out_len);

#endif
