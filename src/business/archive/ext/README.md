# External Interface Layer — `ext/`

## 职责
提供稳定的公共 API，所有外部调用方（ask.c, agent_loop.c）只依赖此层。

## 文件
| 文件 | 行数 | 说明 |
|------|------|------|
| `archive.h` | 164 | 完整公共 API 声明 |

## 接口清单
| 函数 | 用途 | 对应层 |
|------|------|--------|
| `archive_init()` | 初始化 | compat |
| `archive_add_topic()` | 添加/更新 L1 话题 | compat→core→exec |
| `archive_find_topic()` | 按名称查找话题 | compat→core |
| `archive_topics_line()` | 格式化 Topics 行 | compat→core |
| `archive_bump_recall()` | 回访加强 | compat→core→exec |
| `archive_decay_all()` | 强度衰减 | compat→core→exec |
| `archive_store_detail()` | 存储 L3 细节 | compat→exec |
| `archive_append_log()` | 追加 L5 归档 | compat→exec |
| `archive_recall()` | 搜索记忆 | compat→core+exec |
| `archive_semantic_store()` | 存储 L4 语义 | compat→exec |
| `archive_semantic_list()` | 列出语义知识 | compat→exec |
| `archive_handle_directive()` | 处理 `[ARCHIVE:]` | compat |
| `archive_consolidate()` | 知识凝练 | compat→core+exec |
| `archive_feed_turn()` | 喂入对话轮次 | compat→core |
| `archive_detect_topic_shift()` | 检测话题漂移 | compat→core |
| `archive_flush_segment()` | 刷新当前段 | compat→core→exec |

## 不变量
- **永不依赖** core/ 或 exec/ 的实现细节
- 函数签名在 core/exec 变更时**不改变**
- 外部调用方只包含 `"archive.h"`，不包含任何内部头文件
