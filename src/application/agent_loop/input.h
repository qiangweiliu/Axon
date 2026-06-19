/* input.h — Raw-mode terminal input with UTF-8 backspace support */
#ifndef AGENT_INPUT_H
#define AGENT_INPUT_H

void raw_on(void);
void raw_off(void);
int  read_line_raw(char *buf, int max);

#endif /* AGENT_INPUT_H */
