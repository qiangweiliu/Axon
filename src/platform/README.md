# Platform Layer — OS API 抽象层

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

平台抽象层，封装操作系统差异。所有上层代码基于 `os_*` API 编写，不直接使用 POSIX/winsock。当前仅实现 POSIX（Linux/macOS/WSL）后端。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| os_api.h | src/platform/ | 统一 OS API 声明（函数表 + inline 封装） |
| os_api_posix.c | src/platform/ | POSIX 实现（Linux/WSL/macOS） |
| os_api_windows.c | src/platform/ | Windows 实现（占位，未完成） |
| Kconfig | src/platform/ | 编译配置 |
| Makefile | src/platform/ | 构建规则 |

## 三、API 分类

### 内存
- `os_alloc / os_calloc / os_realloc / os_free` — 堆分配

### 文件 / IO
- `os_file_open / os_file_close / os_file_read / os_file_write`
- `os_fprintf_stderr / os_printf` — 输出
- `os_dir_create / os_dir_remove`

### 字符串
- `os_strlen / os_strcmp / os_strncmp / os_strchr`
- `os_memcpy / os_memset`
- `os_snprintf / os_vsnprintf`

### 线程 / 同步
- `os_thread_create / os_thread_join / os_thread_detach`
- `os_mutex_create / os_mutex_lock / os_mutex_unlock / os_mutex_destroy`
- `os_cond_create / os_cond_wait / os_cond_signal / os_cond_broadcast / os_cond_destroy`

### 进程 / IPC
- `os_proc_fork / os_proc_exec`
- `os_pipe / os_dup2 / os_fd_close`
- `os_signal_set`
- `os_file_handle_t / os_pid_t / os_thread_handle_t`

### 系统
- `os_clock_ms` — 毫秒时钟
- `os_sysconf` — 系统配置
- `os_qsort` — 排序

## 四、设计原则

- 所有 `os_*` 函数在 posix 后端由 `#define` 或 wrapper 实现
- 错误返回码: 0 = 成功，-1 = 失败（POSIX 约定）
- 线程/同步函数返回 `os_thread_handle_t`、`os_mutex_handle_t` 等不透明句柄
