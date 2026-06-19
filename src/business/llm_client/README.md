# LLM Client Module — LLM API 客户端

版本: 2.0
日期: 2026-06-19
状态: 已实现

## 一、概述

LLM API 客户端模块（Business 层）。支持流式（SSE）和非流式调用。通过模型适配器模式支持多种模型后端。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| llm_client.h | src/business/llm_client/ | 公共 API |
| llm_client.c | src/business/llm_client/ | 核心层：HTTP 调度 + SSE 解析 + 模块注册 |
| llm_core.h | src/business/llm_client/ | 模型适配器接口（虚表） |
| model_deepseek.c | src/business/llm_client/ | DeepSeek 适配器（content + reasoning_content） |
| model_agnes.c | src/business/llm_client/ | Agnes 适配器（标准 OpenAI 格式） |

## 三、架构

```
LLM Client Core (llm_client.c)
  ├── HTTP dispatch (http_client)
  ├── SSE parser (sse_feed)
  └── Model dispatcher (llm_model_select)
        ├── model_deepseek.c   ← 自动注册 via LLM_MODEL_REGISTER
        └── model_agnes.c      ← 自动注册 via LLM_MODEL_REGISTER
```

### 模型适配器接口

```c
typedef struct {
    const char *name;  // 模型名前缀，如 "deepseek"
    int (*build_body)(...);           // 构建请求 JSON
    char* (*extract_content)(...);    // 从响应 JSON 提取文本
    int (*extract_int)(...);          // 从响应 JSON 提取整数
} llm_model_t;

// 模型自注册宏
#define LLM_MODEL_REGISTER(var)     AGENT_SECTION("llm_models", const llm_model_t, var)
```

## 四、API

```c
// 非流式调用
llm_response_t *llm_chat(const char *endpoint, const char *api_key,
                         const char *model, const char *prompt);

// 流式调用（SSE）
llm_response_t *llm_chat_stream(const char *endpoint, const char *api_key,
                                const char *model, const char *prompt,
                                llm_token_cb_t on_token, void *user);

// 释放响应
void llm_response_free(llm_response_t *resp);
```

## 五、模块注册

- name: "llm_client"
- layer: BUSINESS
- init: 扫描 `.llm_models` section 发现已注册的模型适配器
