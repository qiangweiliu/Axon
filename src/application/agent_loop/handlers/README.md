# handlers — 命令处理器

## 职责
所有 REPL 命令的具体实现。

## 文件
| 文件 | 说明 |
|------|------|
| handlers/handlers.c | 8 个处理函数 |
| handlers/handlers.h | 函数声明 |

## 命令清单
| 函数 | 对应命令 | 说明 |
|------|---------|------|
| handle_note | note | 保存到 MEMORY.md |
| handle_profile | profile | 保存到 USER.md |
| handle_notes | notes | 列出所有记忆 |
| handle_replace | replace | 替换记忆条目 |
| handle_forget | forget | 删除记忆条目 |
| handle_echo | echo | 调用 echo 工具测试 |
| handle_remember | remember | 结构化记忆入库 |
| handle_recall | recall | 搜索结构化记忆 |
