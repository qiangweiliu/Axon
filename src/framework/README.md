# Framework Layer — 框架核心

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

框架核心层，提供模块生命周期管理、ELF section 模块发现、事件总线、内存监控等基础设施。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| agent_framework.h | src/framework/ | 公共 API 头文件（模块、事件、日志、内存） |
| framework_internal.h | src/framework/ | 内部声明（registry、生命周期） |
| framework_main.c | src/framework/ | main() 入口 + framework_init/shutdown |
| registry.c | src/framework/ | ELF section 模块发现 + 注册表 |
| lifecycle.c | src/framework/ | init/start/stop/deinit 生命周期 |
| bus.c | src/framework/ | 事件总线（pub/sub） |
| allocator.c | src/framework/ | 内存分配器 + 泄漏检测 |
| monitor.c | src/framework/ | 运行时监控信息输出 |

## 三、核心机制

### 模块发现
- 每个模块调用 `MODULE_REGISTER(mod)` 将指针放入 `.agent_modules` ELF section
- 启动时 `framework_discover_modules()` 扫描该 section
- 发现顺序 = 链接顺序（由 Makefile 的 LAYERS 变量控制）
```
LAYERS = src/platform src/framework src/foundation src/business src/application
```

### 生命周期
```
framework_init()
  ├─ framework_discover_modules()   — 扫描 section
  ├─ call_init_all()                — 按链接顺序 init
  └─ call_start_all()              — 按链接顺序 start

framework_shutdown()
  ├─ call_stop_all()               — 逆序 stop
  └─ call_deinit_all()             — 逆序 deinit
```

### 事件总线
- 发布/订阅模式，支持优先级
- 预定义事件：`FW_EVENT_CONFIG_LOADED`、`FW_EVENT_START_DONE`、`FW_EVENT_SHUTDOWN`

### 模块结构体
```c
typedef struct framework_module {
    const char *name;     // 模块名
    unsigned int version; // 版本号
    int (*init)(...);     // 初始化
    int (*start)(...);    // 启动
    void (*loop)(...);    // 每帧回调
    int (*stop)(...);     // 停止
    int (*deinit)(...);   // 反初始化
    void *ctx;            // 模块私有上下文
    ...
} framework_module_t;
```

### 日志宏
- `LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR / LOG_FATAL`
- 自动绑定当前模块名

## 四、优先级约定

| 层 | 基值 | 模块 |
|----|------|------|
| Application | 0-99 | agent_loop |
| Business | 400-499 | memory, tool_manager, llm_client, http_client |
| Infrastructure | 700-799 | threadpool |
| Core | 900-999 | config, logger |
