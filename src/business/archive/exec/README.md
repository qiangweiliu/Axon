# Execution Layer — `exec/`

## 职责
具体 I/O 实现。**零业务逻辑**。所有函数只做一件事：读/写某种存储后端。

## 文件
| 文件 | 行数 | 后端 | 说明 |
|------|------|------|------|
| `topics_file.c` | 93 | `memories/topics.md` | 管道分隔文件，每条目 8 字段 |
| `topics_file.h` | 20 | — | 加载/保存接口 |
| `events.c` | 20 | `data/events/{id}.json` | L3 事件详情写入 |
| `log_store.c` | 37 | `data/archive/{date}/{session}.log` | L5 原始对话归档 |
| `semantic.c` | 70 | `data/memory.db` type=semantic | L4 语义知识存储/检索 |

## 替换策略
想换存储后端只需：

| 当前 | 替换为 | 改什么 |
|------|--------|--------|
| `topics_file.c` (topics.md) | SQLite | 重写 `topics_file.c`，接口不变 |
| `events.c` (JSON) | SQLite | 重写 `events.c` |
| `log_store.c` (文件) | syslog | 重写 `log_store.c` |
| `semantic.c` (memory.db) | 向量数据库 | 重写 `semantic.c` |

业务代码、core 层、archive.h **都不需要改**。

## 不变量
- **不允许** 包含 `strength.h`、`topics.h` 等 core 层文件
- **不允许** 直接调用 `archive_add_topic()` 等公共 API
- 函数签名由对应 `*_file.h` 或 `*_store.h` 定义
