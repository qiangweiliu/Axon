/* input.h — Raw-mode terminal input with UTF-8 backspace support,
 *            cursor movement, and command history (up/down arrow).
 *
 * Usage:
 *   raw_on();                     // enable raw mode (no echo, no canonical)
 *   char buf[2048];
 *   int n = read_line_raw(buf, sizeof(buf));  // blocks until Enter
 *   history_add(buf);             // save to history for future up-arrow
 *   raw_off();
 */
#ifndef AGENT_INPUT_H
#define AGENT_INPUT_H

void raw_on(void);
void raw_off(void);
int  read_line_raw(char *buf, int max);

/* Add a command to history (called by REPL after processing each line). */
void history_add(const char *line);

#endif /* AGENT_INPUT_H */
