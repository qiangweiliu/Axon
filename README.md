# Axon

C11 AI Agent 框架。模块化五层架构，ELF section 自注册，无锁异步日志，流式 LLM 调用。

## 架构

```
src/
├── platform/       OS API 抽象层（POSIX/Windows）
├── framework/      框架核心（生命周期、事件总线、模块发现）
├── foundation/     基础服务
│   ├── logger/     无锁异步日志
│   ├── config/     YAML 配置解析
│   └── threadpool/ 线程池 + 并行 init
├── business/       业务模块
│   ├── memory/     持久化记忆（文件 DB）
│   ├── tool_manager/  LLM 工具注册与调用
│   ├── http_client/   HTTP(S) 客户端
│   └── llm_client/    LLM API 客户端 + 模型适配器
└── application/    应用层
    └── agent_loop/ REPL 交互循环 + 记忆文件
```

## 构建

```bash
make                 # 编译 build/agent
make clean           # 清理
```

## 运行

```bash
build/agent          # 交互式 REPL
```

## init 顺序

由链接顺序决定（Makefile LAYERS 变量）:
```
platform → framework → foundation → business → application
```

各模块文档见各子目录 `README.md`。

## 依赖

- gcc (c11)
- openssl（HTTPS 客户端）
- Linux / WSL
