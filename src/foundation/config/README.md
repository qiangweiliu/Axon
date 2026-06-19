# Config Module — 配置管理

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

配置模块，解析 `config.yml` 文件，提供全局只读配置查询接口。支持双层路径（自动将 `llm.endpoint` 映射为 `llm_endpoint`）。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| config.h | src/foundation/config/ | 公共 API + config_t 结构体 |
| config.c | src/foundation/config/ | YAML 解析 + 模块注册 |

## 三、config_t 结构体

```c
typedef struct {
    char log_file[256];       // 日志文件路径（默认 agent.log）
    int  log_level;           // 日志级别（0=debug, 1=info, ...）
    int  threadpool_workers;  // 线程池工作线程数（0=auto）
    char llm_endpoint[256];   // LLM API 端点
    char llm_api_key[256];    // API key
    char llm_model[128];      // 模型名
} config_t;
```

## 四、API

```c
const config_t *config_get(void);
```
返回全局配置指针，只读。启动时从 `config.yml` 加载，发布 `FW_EVENT_CONFIG_LOADED` 事件。

## 五、模块注册

- name: "config"
- layer: CORE（最优先 init）
- init: 解析 config.yml → 配置 logger → 订阅 START_DONE 事件
- start: 发布 CONFIG_LOADED 事件
