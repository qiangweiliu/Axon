# Logger Module — 设计文档

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

无锁异步日志模块（Foundation 层，priority=10）。提供模块级日志级别控制，双后端（直接输出 / 环形缓冲+独立线程）。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| logger.h | src/foundation/logger/ | 公共 API（日志级别、后端切换） |
| logger.c | src/foundation/logger/ | 实现 + MODULE_REGISTER |

## 三、数据结构

环形缓冲区（无锁，纯 `__atomic` CAS）:
- 1024 槽 × 4096 字节/槽
- 三指针: head（生产者）、tail（消费者）、count（原子计数）
- 无 mutex / condvar，避免死锁

模块级别表:
- 最多 64 个模块名，各自独立日志级别

## 四、API

```c
/* 初始化，log_file 可为 NULL（仅 stderr） */
int fw_log_init(const char *log_file, fw_log_level_t level);

/* 设置特定模块的日志级别 */
int fw_log_set_level(const char *name, fw_log_level_t level);

/* 切换后端：NONE（直接 printf）/ BUFFERED（环形缓冲+线程） */
void fw_log_switch(fw_log_backend_t backend);

/* 关闭日志系统 */
void fw_log_shutdown(void);

/* 绑定当前模块名（生命周期回调前自动调用） */
void fw_log_bind_module(const char *name);
const char *fw_log_get_module(void);

/* 日志宏 */
LOG_DEBUG(fmt, ...) / LOG_INFO / LOG_WARN / LOG_ERROR / LOG_FATAL
```

## 五、后端切换

- 默认 NONE：`LOG_*` 直接 printf 到 stderr
- `fw_log_init()` 自动切换到 BUFFERED：启动独立日志线程，每 2ms 轮询环形缓冲
- `fw_log_shutdown()` 切回 NONE + 排空缓冲

## 六、模块注册

- name: "logger"
- priority: 10（Foundation 层，最低优先级编号，最早初始化）
- init/start: 无操作（fw_log_init 在 framework_main.c 中提前调用）
