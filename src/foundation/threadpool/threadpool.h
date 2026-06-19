/*
 * threadpool.h — 线程池模块公共 API
 *
 * Foundation 层 (priority=20)，在 logger (10) 之后初始化。
 * 自动检测 CPU 核数，为模块并行加载提供执行能力。
 */

#ifndef AGENT_THREADPOOL_H
#define AGENT_THREADPOOL_H

#include <stddef.h>

/* 不透明结构，定义在 threadpool.c */
typedef struct threadpool threadpool_t;

/* 全局线程池实例 — 模块 init 后设置，NULL 表示未就绪 */
extern threadpool_t *g_threadpool;

/* 任务函数签名：返回 0 成功，非 0 失败 */
typedef int (*threadpool_task_fn_t)(void *arg);

/* 获取 CPU 核数（通过 os_sysconf） */
int threadpool_get_cpu_count(void);

/* 创建线程池，worker 数量为 num_workers */
threadpool_t *threadpool_create(int num_workers);

/* 销毁线程池，等待所有 worker 退出并释放资源 */
void threadpool_destroy(threadpool_t *pool);

/*
 * 提交任务到线程池。
 * fn 返回 0 成功，非 0 失败；结果写入 *result_out。
 * 返回 0 成功，-1 队列已满。
 */
int threadpool_submit(threadpool_t *pool,
                      threadpool_task_fn_t fn,
                      void *arg,
                      int *result_out);

/* 阻塞等待所有已提交任务完成 */
int threadpool_wait(threadpool_t *pool);

/* 返回当前待处理任务数 */
int threadpool_pending(threadpool_t *pool);

#endif /* AGENT_THREADPOOL_H */
