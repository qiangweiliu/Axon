# Core Layer — `core/`

## 职责
纯业务逻辑实现。**零 I/O、零操作系统调用**。可单元测试。

## 文件
| 文件 | 行数 | 类型 | 说明 |
|------|------|------|------|
| `strength.c` | 49 | Pure math | 强度分计算 + 衰减 + 淘汰策略 |
| `strength.h` | 23 | Header | 纯函数声明 |
| `topics.c` | 194 | Core logic | L1 话题 CRUD、查询、缓存、Prompt 格式化 |
| `segment.c` | 112 | Core logic | 事件分割、关键词漂移检测、段刷新 |
| `keywords.c` | 70 | Pure alg | 中文/英文关键词提取、重叠比计算 |
| `keywords.h` | 21 | Header | 纯函数声明 |

## 强度分公式（strength.c）
```
score = (importance × W_IMPORTANCE / 100)
      + recency_bonus(days)
      + recall_count × W_FREQUENCY
      - days × W_DECAY
```
其中 `W_IMPORTANCE=30`, `W_RECENCY=4`, `W_FREQUENCY=4`, `W_DECAY=5`

## 不变量
- **不允许** `#include "os_api.h"` 或 `#include "memory.h"`
- **不允许** 任何 `os_file_*` 调用
- 文件 I/O 委托给 `exec/` 层
- keywords.c 是**纯函数**，零依赖（可独立编译测试）

## 依赖关系
```
topics.c → strength.h (纯函数)
topics.c → topics_file.h (exec 层接口)
segment.c → keywords.h (纯函数)
segment.c → ext/archive.h (通过公共接口委托持久化)
```
