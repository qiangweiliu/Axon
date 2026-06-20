# Archive Module — 分层记忆归档

## 架构总览

```
External Interface (ext/archive.h) — 稳定公共 API
       │
Compatibility Layer (compat/) — 协调映射
       │
┌──────┴──────┐
Core (core/)  Exec (exec/)
纯逻辑         具体 I/O
无 I/O         零业务逻辑
       │
       └── 可互换存储后端
```

## 分层约束

| 层 | 目录 | 允许的 include | 禁止的 include |
|---|------|---------------|----------------|
| External | `ext/` | —（头文件） | — |
| Compat | `compat/` | 任意 | — |
| Core | `core/` | `strength.h` `keywords.h` `ext/archive.h` | `os_api.h` `memory.h` |
| Exec | `exec/` | `os_api.h` `memory.h` | `core/*` |

## 数据流示例

```
用户问问题 → ask.c 调 archive_topics_line()
  → compat/archive.c 调 core/topics.c 的 topics_get_line()
  → core/topics.c 格式化缓存的 topics_line
  → 返回字符串给 prompt

LLM 输出 [ARCHIVE: topic=xxx | episode=... | importance=high]
  → ask.c 调 archive_handle_directive()
  → compat/directives.c 解析 key=value
  → 调 archive_add_topic() → compat/archive.c → core/topics.c
    → core/topics.c 调 strength.h 算分
    → core/topics.c 调 topics_file.h 写文件
  → 调 archive_store_detail() → compat/archive.c → exec/events.c
    → exec/events.c 写 data/events/{id}.json
```

## 文件统计

```
ext/     archive.h      164 行
compat/  archive.c      112 行
         directives.c    99 行
core/    strength.c      49 行
         topics.c       194 行
         segment.c      112 行
         keywords.c      70 行
exec/    topics_file.c   93 行
         events.c        20 行
         log_store.c     37 行
         semantic.c      70 行
other    archive_internal.h  87 行
         Kconfig             10 行
         Makefile             8 行
───────────────────────────────
总计     11 文件        ~1100 行
```
