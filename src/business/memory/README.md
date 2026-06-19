# Memory Module — 持久化记忆

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

结构化记忆存储模块（Business 层）。提供基于文件的键值存储，支持增删查改。后端可插拔，当前仅实现文件后端。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| memory.h | src/business/memory/ | 公共 API + memory_backend_t 接口 |
| memory.c | src/business/memory/ | 模块注册 + 后端调度 |
| backend_file.c | src/business/memory/ | 文件后端实现（data/memory.db） |

## 三、API

```c
// 存储一条记忆，id_out 返回生成的唯一 ID
int memory_store(const memory_entry_t *entry, char *id_out, size_t id_len);

// 按 ID 读取
int memory_read(const char *id, memory_entry_t *out);

// 搜索（返回最多 4 条）
int memory_search(const char *query, memory_entry_t *results, int max_results);

// 删除
int memory_delete(const char *id);
```

## 四、模块注册

- name: "memory"
- layer: BUSINESS
- init: 初始化文件后端（创建 data/ 目录 + 数据库文件）
- start: 确认后端就绪
