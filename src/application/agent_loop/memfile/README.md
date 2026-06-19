# memfile — 有界记忆文件

## 职责
Bounded markdown 记忆文件，Hermes 风格。按行存条目，上限可配。

## 文件
| 文件 | 说明 |
|------|------|
| memfile/memfile.c | 实现：load/add/replace/remove/save/usage |
| memfile/memfile.h | 公共 API |

## 限制
- 最大条目数：64
- 单条最大长度：1024 字符
- Memory 总上限：8000 字符
- User 总上限：4000 字符
