# HTTP Client Module — HTTP(S) 客户端

版本: 1.0
日期: 2026-06-19
状态: 已实现

## 一、概述

HTTP/HTTPS 客户端模块（Business 层）。HTTP 通过原生 socket 实现。HTTPS 通过管道到 `openssl s_client`（零库依赖）。

## 二、文件清单

| 文件 | 路径 | 职责 |
|------|------|------|
| http_client.h | src/business/http_client/ | 公共 API |
| http_client.c | src/business/http_client/ | HTTP/HTTPS 请求 + 响应解析 + chunked 解码 |

## 三、API

```c
// HTTP POST（非流式）
http_response_t *http_post(const char *host, int port, const char *path,
                           const char *content_type, const char *body,
                           const char *extra_headers);

// HTTPS POST（非流式）
http_response_t *https_post(const char *host, int port, const char *path,
                            const char *content_type, const char *body,
                            const char *extra_headers);

// HTTPS POST（流式 SSE）
int https_post_stream(const char *host, int port, const char *path,
                      const char *content_type, const char *body,
                      const char *extra_headers,
                      http_chunk_cb_t on_chunk, void *user);

// 释放响应
void http_response_free(http_response_t *resp);
```

## 四、响应结构体

```c
typedef struct {
    int status_code;    // HTTP 状态码
    char *body;         // 响应体（malloc'd）
    size_t body_len;    // 响应体长度
} http_response_t;
```

## 五、模块注册

- name: "http_client"
- layer: BUSINESS
- init/start: 空操作（模块无状态）
