# ThreadPool Module — 设计文档

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

线程池模块（Foundation 层，LAYER_INFRA）。自动检测 CPU 核心数，创建常驻工作线程。
框架 lifecycle.c 在 init/start 阶段检测 g_threadpool，自动切换并行执行剩余模块。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| threadpool.h | src/foundation/threadpool/ | 公共 API 声明 |
| threadpool.c | src/foundation/threadpool/ | 实现 + MODULE_REGISTER |

## 三、数据结构

```
threadpool_t (不透明)
├── task_t tasks[MAX_TASKS=256]    固定大小环形任务队列
├── int head / tail / count          队列指针 + 计数
├── os_mutex_handle_t mutex         保护队列
├── os_cond_handle_t cond_worker    唤醒空闲 worker
├── os_cond_handle_t cond_done      通知 wait() 调用者
├── os_thread_handle_t *workers     工作线程数组
├── int num_workers                 线程数 = CPU 核数
├── int running                     关闭标志
├── int pending                     已提交未完成计数
└── int completed                   已完成计数
```

## 四、API

```c
/* 全局线程池实例（模块 init 后设置，生命周期内可用） */
extern threadpool_t *g_threadpool;

/* 获取 CPU 核心数 */
int threadpool_get_cpu_count(void);

/* 创建线程池，workers 为线程数 */
threadpool_t *threadpool_create(int num_workers);

/* 销毁线程池，等待所有 worker 退出 */
void threadpool_destroy(threadpool_t *pool);

/* 提交任务 */
int threadpool_submit(threadpool_t *pool,
                      int (*fn)(void *arg),
                      void *arg,
                      int *result_out);

/* 阻塞等待所有已提交任务完成 */
int threadpool_wait(threadpool_t *pool);

/* 查询待处理任务数 */
int threadpool_pending(threadpool_t *pool);
```

## 五、模块注册

- name: "threadpool"
- layer: INFRA
- init: 调用 threadpool_create(CPU_COUNT)，设置 g_threadpool
- start: 确认池已就绪
- stop/deinit: threadpool_destroy

## 六、与生命周期集成

框架 lifecycle.c 在 init/start 阶段检测 g_threadpool：
- 顺序 init 直到 threadpool 模块完成初始化
- 之后将所有剩余模块的 init/start 提交到线程池并行执行
- wait 收集结果，检测失败并回滚

## 七、线程安全

- 任务队列由 mutex + cond 保护
- worker 线程通过 cond_worker 等待任务
- submit 唤醒一个 worker
- wait 通过 cond_done 等待 pending==0
- 注意：fw_log_bind_module() 使用全局变量，并行阶段日志模块名可能不精确

## 八、配置

无外部配置。线程数 = sysconf(_SC_NPROCESSORS_ONLN)，通过 os_sysconf() 封装。
