/*
 * threadpool.c — 固定大小线程池实现
 *
 * Foundation 层 (priority=20)
 * 自动检测 CPU 核数，创建对应数量的常驻工作线程。
 * 任务通过环形队列分发，支持 submit / wait / pending 查询。
 *
 * 所有系统调用通过 os_api，不使用任何 C 标准库头文件。
 */

#include "agent_framework.h"
#include "framework_internal.h"
#include "os_api.h"
#include "threadpool.h"
#include "config.h"

/* =========================================================================
 * 内部常量
 * ========================================================================= */

#define MAX_TASKS   256       /* 环形队列容量 */
#define MAX_WORKERS 64        /* 最大线程数 */

/* =========================================================================
 * 任务条目
 * ========================================================================= */

typedef struct {
    threadpool_task_fn_t fn;
    void                *arg;
    int                 *result;   /* 结果写入此地址 */
} task_t;

/* =========================================================================
 * 线程池结构体
 * ========================================================================= */

struct threadpool {
    task_t              tasks[MAX_TASKS];
    int                 head;
    int                 tail;
    int                 count;
    os_mutex_handle_t   mutex;
    os_cond_handle_t    cond_worker;   /* 唤醒空闲 worker */
    os_cond_handle_t    cond_done;     /* 通知 wait() 所有任务完成 */
    os_thread_handle_t  *workers;
    int                 num_workers;
    int                 running;
    int                 pending;
    int                 completed;
};

/* 全局实例 — 模块 init 后设置 */
threadpool_t *g_threadpool = NULL;

/* =========================================================================
 * CPU 核数检测
 * ========================================================================= */

int threadpool_get_cpu_count(void)
{
    long n = os_sysconf(84);  /* _SC_NPROCESSORS_ONLN = 84 */
    return (n > 0) ? (int)n : 1;
}

/* =========================================================================
 * Worker 线程主循环
 * ========================================================================= */

static void *worker_loop(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    for (;;) {
        os_mutex_lock(pool->mutex);

        /* 等待任务或关闭信号 */
        while (pool->count == 0 && pool->running) {
            os_cond_wait(pool->cond_worker, pool->mutex, -1);
        }

        /* 关闭且队列空 → 退出 */
        if (!pool->running && pool->count == 0) {
            os_mutex_unlock(pool->mutex);
            break;
        }

        /* 取出一个任务 */
        task_t t = pool->tasks[pool->head];
        pool->head = (pool->head + 1) % MAX_TASKS;
        pool->count--;

        os_mutex_unlock(pool->mutex);

        /* 执行任务 */
        int rc = t.fn(t.arg);
        if (t.result) {
            *t.result = rc;
        }

        /* 更新完成计数 */
        os_mutex_lock(pool->mutex);
        pool->completed++;
        pool->pending--;
        if (pool->pending == 0) {
            os_cond_signal(pool->cond_done);
        }
        os_mutex_unlock(pool->mutex);
    }

    return NULL;
}

/* =========================================================================
 * 创建 / 销毁
 * ========================================================================= */

threadpool_t *threadpool_create(int num_workers)
{
    if (num_workers < 1) num_workers = 1;
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;

    threadpool_t *pool = (threadpool_t *)os_calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->mutex = os_mutex_create();
    if (!pool->mutex) { os_free(pool); return NULL; }

    pool->cond_worker = os_cond_create();
    if (!pool->cond_worker) { os_mutex_destroy(pool->mutex); os_free(pool); return NULL; }

    pool->cond_done = os_cond_create();
    if (!pool->cond_done) {
        os_cond_destroy(pool->cond_worker);
        os_mutex_destroy(pool->mutex);
        os_free(pool);
        return NULL;
    }

    pool->workers = (os_thread_handle_t *)os_calloc((size_t)num_workers,
                                                     sizeof(os_thread_handle_t));
    if (!pool->workers) {
        os_cond_destroy(pool->cond_done);
        os_cond_destroy(pool->cond_worker);
        os_mutex_destroy(pool->mutex);
        os_free(pool);
        return NULL;
    }

    pool->num_workers = num_workers;
    pool->running = 1;
    pool->head = 0;
    pool->tail = 0;
    pool->count = 0;
    pool->pending = 0;
    pool->completed = 0;

    /* 创建 worker 线程（不在锁内创建 — 避免子线程立刻竞争同一把锁） */
    for (int i = 0; i < num_workers; i++) {
        int rc = os_thread_create(&pool->workers[i], worker_loop, pool);
        if (rc != 0) {
            /* 部分创建失败 — 通知已创建的退出 */
            os_mutex_lock(pool->mutex);
            pool->running = 0;
            os_cond_signal(pool->cond_worker);
            os_mutex_unlock(pool->mutex);
            /* 加入已创建的线程 */
            for (int j = 0; j < i; j++) {
                os_thread_join(pool->workers[j]);
            }
            os_free(pool->workers);
            os_cond_destroy(pool->cond_done);
            os_cond_destroy(pool->cond_worker);
            os_mutex_destroy(pool->mutex);
            os_free(pool);
            return NULL;
        }
    }

    return pool;
}

void threadpool_destroy(threadpool_t *pool)
{
    if (!pool) return;

    /* 通知所有 worker 退出 */
    os_mutex_lock(pool->mutex);
    pool->running = 0;
    /* 广播唤醒所有阻塞在 cond_worker 上的 worker */
    for (int i = 0; i < pool->num_workers; i++) {
        os_cond_signal(pool->cond_worker);
    }
    os_mutex_unlock(pool->mutex);

    /* 等待所有 worker 退出 */
    for (int i = 0; i < pool->num_workers; i++) {
        os_thread_join(pool->workers[i]);
    }

    os_free(pool->workers);
    os_cond_destroy(pool->cond_worker);
    os_cond_destroy(pool->cond_done);
    os_mutex_destroy(pool->mutex);
    os_free(pool);
}

/* =========================================================================
 * 任务提交 / 等待
 * ========================================================================= */

int threadpool_submit(threadpool_t *pool,
                      threadpool_task_fn_t fn,
                      void *arg,
                      int *result_out)
{
    if (!pool || !fn) return -1;

    os_mutex_lock(pool->mutex);

    if (pool->count >= MAX_TASKS) {
        os_mutex_unlock(pool->mutex);
        return -1;  /* 队列满 */
    }

    task_t *t = &pool->tasks[pool->tail];
    t->fn = fn;
    t->arg = arg;
    t->result = result_out;
    pool->tail = (pool->tail + 1) % MAX_TASKS;
    pool->count++;
    pool->pending++;

    /* 唤醒一个 worker */
    os_cond_signal(pool->cond_worker);
    os_mutex_unlock(pool->mutex);

    return 0;
}

int threadpool_wait(threadpool_t *pool)
{
    if (!pool) return -1;

    os_mutex_lock(pool->mutex);

    while (pool->pending > 0) {
        os_cond_wait(pool->cond_done, pool->mutex, -1);
    }

    os_mutex_unlock(pool->mutex);
    return 0;
}

int threadpool_pending(threadpool_t *pool)
{
    if (!pool) return 0;

    os_mutex_lock(pool->mutex);
    int n = pool->pending;
    os_mutex_unlock(pool->mutex);

    return n;
}

/* =========================================================================
 * 模块注册 (priority=20, Foundation 层)
 * ========================================================================= */

static void on_config_loaded(framework_event_type_t type,
                             const void *data, size_t data_size,
                             void *user_data);

static int threadpool_init(framework_module_t *mod)
{
    (void)mod;

    const config_t *cfg = config_get();
    int workers = (cfg && cfg->threadpool_workers > 0)
                  ? cfg->threadpool_workers
                  : threadpool_get_cpu_count();

    LOG_INFO("ThreadPool: using %d worker(s)%s",
             workers, (cfg && cfg->threadpool_workers > 0) ? " (from config)" : " (auto)");

    g_threadpool = threadpool_create(workers);
    if (!g_threadpool) {
        LOG_ERROR("ThreadPool: failed to create pool with %d workers", workers);
        return -1;
    }

    LOG_INFO("ThreadPool: pool created with %d worker(s)", workers);

    /* Subscribe to config load events */
    framework_event_subscribe(FW_EVENT_CONFIG_LOADED, on_config_loaded, 0, NULL);

    return 0;
}

static void on_config_loaded(framework_event_type_t type,
                             const void *data, size_t data_size,
                             void *user_data)
{
    (void)type; (void)data_size; (void)user_data;
    const config_t *cfg = (const config_t *)data;
    if (cfg) {
        LOG_INFO("ThreadPool: event CONFIG_LOADED "
                 "(workers=%d, log_level=%d)",
                 cfg->threadpool_workers, cfg->log_level);
    }
}

static int threadpool_start(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("ThreadPool: ready");
    return 0;
}

static int threadpool_stop(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("ThreadPool: stopping");
    return 0;
}

static int threadpool_deinit(framework_module_t *mod)
{
    (void)mod;
    LOG_INFO("ThreadPool: destroying pool");
    threadpool_destroy(g_threadpool);
    g_threadpool = NULL;
    return 0;
}

    framework_module_t threadpool_mod = {
    .name    = "threadpool",
    .version = 0x00010000,
    
    .state   = FRAMEWORK_STATE_UNLOADED,
    .init    = threadpool_init,
    .start   = threadpool_start,
    .loop    = NULL,
    .stop    = threadpool_stop,
    .deinit  = threadpool_deinit,
    .ctx     = NULL,
    .id      = 0,
    .next    = NULL,
};

MODULE_REGISTER(threadpool_mod, LAYER_INFRA, 0);
