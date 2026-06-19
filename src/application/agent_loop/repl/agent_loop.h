/*
 * agent_loop.h — Agent decision loop
 *
 * Two modes:
 *   agent_loop_init/start/loop  — prompt.txt file mode (framework-driven)
 *   agent_loop_repl()           — interactive stdin mode (blocking)
 */

#ifndef APPLICATION_AGENT_LOOP_H
#define APPLICATION_AGENT_LOOP_H

/* Enter interactive REPL: reads stdin, sends to LLM, prints response.
   Blocking — only returns on EOF or "exit". Requires framework init first. */
void agent_loop_repl(void);

#endif /* APPLICATION_AGENT_LOOP_H */
