# Tool Manager Module — 工具管理

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

LLM 工具调用管理器（Business 层）。注册和管理可供 LLM 调用的工具。当前内置 `echo` 工具用于测试。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| tool_manager.h | src/business/tool_manager/ | 公共 API + tool_call / tool_call_json |
| tool_manager.c | src/business/tool_manager/ | 工具注册 + 调用分发 + 模块注册 |

## 三、API

```c
// 调用工具，返回 JSON 字符串
int tool_call(const char *name, const char *args_json,
              char *out, size_t out_len);

// 调用工具，返回格式化的 JSON 响应
int tool_call_json(const char *name, const char *args_json,
                   char *out, size_t out_len);

// 注册工具
int tool_register(const tool_t *tool);
```

## 四、模块注册

- name: "tool_manager"
- layer: BUSINESS
- init: 注册内置 `echo` 工具
