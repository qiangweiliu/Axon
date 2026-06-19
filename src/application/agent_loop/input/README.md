# input — 原始终端输入

## 职责
raw 模式终端输入，支持 UTF-8 中文退格。

## 文件
| 文件 | 说明 |
|------|------|
| input/input.c | raw_on/raw_off、read_line_raw |
| input/input.h | 公共 API |

## 函数
- `raw_on()` — 开启 raw 模式（关闭回显、行缓冲）
- `raw_off()` — 恢复终端设置
- `read_line_raw(buf, size)` — 逐字符读取输入，处理退格
