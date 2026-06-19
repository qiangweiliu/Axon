# ask — LLM 交互

## 职责
LLM 流式调用、思维链显示、统计栏、记忆指令回写。

## 文件
| 文件 | 说明 |
|------|------|
| ask/ask.c | spinner、on_llm_token、handle_ask、统计栏、指令解析 |
| ask/ask.h | handle_ask 声明 |

## 关键函数
- `handle_ask(question, out, out_len)` — 构建 prompt → 调用 llm_chat_stream → 显示思维链 + 统计
- `on_llm_token()` — 流式回调：DIM 显示 reasoning、过渡空行、\n 转义
