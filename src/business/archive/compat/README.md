# Compatibility Layer — `compat/`

## 职责
External Interface（archive.h）的**实现层**。负责协调 Core Layer 和 Execution Layer 完成业务功能。包含 LLM 指令解析。

## 文件
| 文件 | 行数 | 说明 |
|------|------|------|
| `archive.c` | 112 | 公共 API 实现（每函数 1 行委托） |
| `directives.c` | 99 | `[ARCHIVE:]` 指令解析 + consolidation |

## 依赖方向
```
compat/archive.c
  → core/topics.c (话题管理)
  → core/strength.c (强度计算)
  → core/segment.c (事件分割)
  → exec/topics_file.c (文件持久化)
  → exec/events.c (事件存储)
  → exec/log_store.c (归档日志)
  → exec/semantic.c (语义知识)

compat/directives.c
  → ext/archive.h (公共 API)
  → exec/semantic.c (consolidation)
```

## 不变量
- **不包含** core 或 exec 的原始业务逻辑
- `archive.c` 的每个函数 ≤ 3 行，纯映射
- 如果 exec 层的存储实现变更（如 topics.md→SQLite），**只改这里**
