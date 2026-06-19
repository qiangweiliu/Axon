# Agent Loop Module — 交互循环

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

应用层模块，提供交互式 REPL 循环。处理用户输入、调用 LLM、管理持久化记忆、显示统计信息。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| agent_loop.c | src/application/agent_loop/ | REPL + 命令处理 + LLM 调用 + 模块注册 |
| agent_loop.h | src/application/agent_loop/ | 公共接口声明 |
| ansi.h | src/application/agent_loop/ | ANSI 终端颜色宏 |
| input.c | src/application/agent_loop/ | raw 模式终端输入（UTF-8 退格支持） |
| input.h | src/application/agent_loop/ | 输入接口声明 |
| memfile.c | src/application/agent_loop/ | 有界记忆文件实现 |
| memfile.h | src/application/agent_loop/ | 记忆文件 API |

## 三、命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `ask <question>` | 向 LLM 发送问题（流式输出） | `ask 今天天气怎么样？` |
| `note <text>` | 保存到持久化记忆（MEMORY.md） | `note 用户喜欢简洁的回答` |
| `profile <text>` | 保存用户档案（USER.md） | `profile Name is 老板鱼饭` |
| `notes` | 列出所有记忆条目 | `notes` |
| `replace <key> <text>` | 替换包含 key 的记忆条目 | `replace 简洁 用户偏好详细` |
| `forget [-m\|-u] <sub>` | 删除包含 sub 的条目 | `forget -m 测试` |
| `echo <msg>` | 调用 echo 工具测试 | `echo hello` |
| `remember <text>` | 存储到结构化记忆 DB | `remember User likes dark mode` |
| `recall <query>` | 搜索结构化记忆 DB | `recall dark mode` |
| `help` | 显示帮助 | `help` |
| `exit` / `quit` | 退出 | `exit` |

## 四、记忆系统

双存储设计：
- **MEMORY.md** — 有界文本文件（8K chars 上限），按行存储条目，注入 LLM prompt
- **USER.md** — 用户档案（4K chars 上限），同上
- **memory.db** — 结构化文件 DB，支持搜索

系统指令注入：每次 `ask` 在 prompt 前注入记忆系统使用说明，LLM 可通过 `[NOTE:]`、`[PROFILE:]`、`[FORGET:]` 标记静默操作记忆。

## 五、模块注册

- name: "agent_loop"
- layer: APPLICATION
- init: 加载记忆文件、初始化终端输入、设置 prompt 记录
- start: 打印 banner
- loop: 每帧 tick（当前为空操作）
