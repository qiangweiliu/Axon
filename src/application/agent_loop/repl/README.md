# repl — REPL Main Loop

## 职责
交互式 REPL 主循环 + 命令分发 + 输入历史 + 启动 Banner + 模块注册。

## 文件
| 文件 | 说明 |
|------|------|
| repl/agent_loop.c | REPL 循环、process_line 分发、history、banner、模块注册 |
| repl/agent_loop.h | 对外接口（agent_loop_repl、agent_set_prompt_file） |

## 依赖
- ask/ — handle_ask / 流式 LLM 调用
- handlers/ — 命令处理器
- memfile/ — 记忆文件加载
- input/ — raw 终端输入
- agent_private.h — 共享 ctx、g_ctx extern
