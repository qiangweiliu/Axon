# AI Agent C Framework — 设计文档

版本: 2.0
日期: 2026-06-20
状态: 已实现

## 一、概述

本项目在 `/home/wu/agent/` 下用 C11 实现一个轻量级 AI Agent 框架。核心思路是将 Agent 的能力拆分为可插拔模块，通过五层架构组织，编译由顶层 Makefile 统一管理。

源码统计: 约 9000 行 (45+ .c/.h 文件)，10 个 framework 模块。

Agent 核心循环: REPL 读取用户输入 → 构建三层 prompt → LLM API 调用 → 解析指令 → 执行动作 → 输出响应 → 循环。

## 二、五层架构

| 层级 | 优先级范围 | 职责 | 调用方向 |
|------|-----------|------|---------|
| Application (应用决策层) | 500-999 | Agent 决策循环、REPL 交互、Prompt 构建、指令解析 | 调用 Business |
| Business (业务逻辑层) | 300-499 | LLM 调用、工具管理、记忆管理、话题归档、技能加载 | 调用 Foundation + Framework |
| Foundation (基础能力层) | 10-299 | 日志(10)、线程池(20)、配置解析(30) | 调用 Framework + Platform |
| Framework (框架基础设施层) | 100-299 | 模块注册表(110)、生命周期管理(120)、事件总线(130)、内存追踪(140) | 调用 Platform |
| Platform (平台适配层) | 1-9 | OS API 封装(文件系统、网络、进程、线程) | 无依赖 |

调用规则: 只能向下调用，事件向上传播。不允许跨层调用。

初始化顺序: Platform → Framework → Foundation → Business → Application
关闭顺序: Application → Business → Foundation → Framework → Platform

## 三、目录结构

```
agent/
├── Makefile                    # 构建入口
├── Kconfig                     # 顶层配置（声明层列表）
├── config.yml                  # 运行时配置
├── agent.ld                    # 链接脚本
├── DESIGN.md                   # 本文档
├── README.md
│
├── src/
│   ├── platform/               # Platform 平台适配层 (1 模块)
│   │   ├── Kconfig / Makefile
│   │   ├── os_api.h            # 统一 OS 接口抽象
│   │   ├── os_api_posix.c      # POSIX 实现
│   │   └── os_api_windows.c    # Windows 存根
│   │
│   ├── framework/              # Framework 框架基础设施层 (4 文件)
│   │   ├── Kconfig / Makefile
│   │   ├── agent_framework.h   # 公共 API：模块、事件、日志、分配器
│   │   ├── framework_internal.h
│   │   ├── framework_main.c    # main() 入口
│   │   ├── registry.c          # 模块注册表
│   │   ├── lifecycle.c         # init→start→loop→stop→deinit
│   │   ├── bus.c               # 事件总线
│   │   └── allocator.c         # 内存分配追踪
│   │
│   ├── foundation/             # Foundation 基础能力层 (3 模块)
│   │   ├── Kconfig / Makefile
│   │   ├── config/             # 配置解析器 (priority=30)
│   │   ├── logger/             # 无锁异步日志 (priority=10)
│   │   └── threadpool/         # 线程池 (priority=20)
│   │
│   ├── business/               # Business 业务逻辑层 (6 模块)
│   │   ├── Kconfig / Makefile
│   │   ├── http_client/        # HTTP 客户端 (priority=340)
│   │   ├── llm_client/         # LLM API 封装 (priority=350)
│   │   │   ├── llm_client.c    # 流式聊天补全 + 指令处理
│   │   │   ├── model_deepseek.c# DeepSeek 模型适配器
│   │   │   └── model_agnes.c   # Agnes 模型适配器
│   │   ├── skill_manager/      # 技能注册 (priority=360)
│   │   ├── archive/            # 分层记忆归档 (priority=360)
│   │   │   ├── ext/archive.h          # External Interface (稳定 API)
│   │   │   ├── compat/archive.c       # Compatibility Layer
│   │   │   ├── compat/directives.c    # LLM 指令解析
│   │   │   ├── core/topics.c          # L1 话题管理 (Core)
│   │   │   ├── core/strength.c        # 强度分计算 (Core, 纯数学)
│   │   │   ├── core/segment.c         # 事件分割 (Core)
│   │   │   ├── core/keywords.c        # 关键词提取 (Core, 纯函数)
│   │   │   ├── exec/topics_file.c     # topics.md 文件 I/O
│   │   │   ├── exec/events.c          # events/*.json 存储
│   │   │   ├── exec/log_store.c       # archive/*.log 归档
│   │   │   └── exec/semantic.c        # memory.db 语义知识
│   │   ├── memory/             # 记忆后端 (priority=400)
│   │   │   ├── memory.c        # 门面 + 后端调度
│   │   │   ├── backend_file.c  # TSV 文件实现
│   │   │   └── memory.h        # 后端抽象接口 + 公共 API
│   │   └── tool_manager/       # 工具注册与执行 (priority=380)
│   │
│   └── application/            # Application 应用决策层 (1 模块)
│       └── agent_loop/         # Agent 交互循环 (priority=500)
│           ├── repl/agent_loop.c   # stdin REPL 交互循环
│           ├── ask/ask.c           # LLM 交互 (prompt构建+指令解析)
│           ├── handlers/handlers.c # NOTE/PROFILE/RECALL 等命令
│           └── memfile/memfile.c   # 有界记忆文件器
│
├── data/
│   ├── skills/                 # 50 个 Hermes 兼容 SKILL.md
│   ├── events/                 # L3 事件详情 JSON
│   ├── archive/                # L5 原始对话日志
│   └── memory.db               # L2/L4 持久化存储
│
└── memories/
    ├── memory.md               # L0 工作记忆 (有界, ~8KB)
    ├── user.md                 # 用户画像 (有界, ~4KB)
    └── topics.md               # L1 话题索引 (有界, ≤50条)
```

## 四、模块注册机制

使用 `__attribute__((constructor(101)))` 实现模块静态注册:

```c
#define MODULE_REGISTER(mod_instance) \
    __attribute__((constructor(101))) \
    static void _fw_ctor_##mod_instance(void) \
    { \
        _fw_module_register(&mod_instance); \
    }
```

每个模块定义一个 `framework_module_t` 实例（含 name, version, state, init/start/loop/stop/deinit 回调），通过 `MODULE_REGISTER()` 宏注册。构造函数优先级 101，确保在默认构造函数之后执行。框架启动时按 priority 降序排序后初始化。

## 五、生命周期管理

五个阶段: init → start → run(loop) → stop → deinit

- **init**: 分配资源、初始化数据结构。失败时逆序 rollback 已 init 的模块。
- **start**: 激活模块（启动线程、连接服务）。失败时逆序 rollback。
- **run**: 框架主循环 `framework_loop_tick()` 遍历所有 RUNNING 模块的 loop 回调。
- **stop**: 逆序停止所有 RUNNING 模块。
- **deinit**: 逆序释放资源。

state 字段: UNLOADED → INITED → STARTED → RUNNING → STOPPING → DEINITED。

## 六、事件总线

发布/订阅模式，`uint32_t` 事件类型 ID:
- 最多 1024 种事件类型，每种最多 128 个订阅者
- 订阅者按 priority 降序接收
- 发布时深拷贝数据(通过 framework_alloc)
- pthread_mutex 保护并发安全

## 七、日志系统

无锁异步日志，两个后端:
- **STDOUT**: printf 直接输出 (init 前使用)
- **BUFFERED**: 无锁环形缓冲区 (RING_N=1024, ENTRY_N=4096) + 独立日志线程(每 2ms 轮询)

使用 `__atomic` CAS 实现 lock-free ring buffer，避免 mutex+condvar 死锁风险。

## 八、内存分配

统一 wrapper: `framework_alloc()` / `framework_free()`。维护 256 条追踪记录，跟踪每次分配的地址、大小、文件名、行号。支持 `framework_leak_report()` 输出泄漏统计。

## 九、Platform 层 OS API 封装

所有系统操作通过 `os_api.h` 统一接口:
- 文件: `os_file_open/close/read/write/seek/tell`
- 目录: `os_dir_open/close/next`, `os_dir_create`
- 网络: `os_socket_create/bind/listen/accept/recv/send/close`
- 进程: `os_proc_fork/exec/wait`
- 线程: `os_thread_create/join/detach`
- 同步: `os_mutex_create/lock/unlock/destroy`, `os_cond_create/signal/wait/destroy`
- 时间: `os_clock_ms/us`, `os_sleep_ms`, `os_time_format`
- 内存: `os_alloc/free`, `os_memset/memcpy`
- 字符串: `os_strlen/strcmp/strncpy/snprintf`
- 环境: `os_env_get/set`
- 信号: `os_signal_set`

上层代码不直接依赖 `<pthread.h>`、`<signal.h>`、`<stdlib.h>`、`<string.h>`、`<stdio.h>` 等系统头文件。所有 libc 函数通过 os_api 封装。

## 十、编译系统

### 三层构建体系

```
Makefile (顶层)         ← 读 Kconfig → include 各层 Makefile → 链接
  └─ layer/Makefile     ← 读 Kconfig → 管理本层模块
       └─ module/Makefile ← 读 Kconfig → 声明源文件
```

### Kconfig 职责

| 文件 | 职责 |
|------|------|
| `Kconfig` (顶层) | 声明有哪些层 (LAYERS) |
| `Makefile` (顶层) | include 层 Makefile，定义通用编译规则，链接 |
| `Kconfig` (层) | 声明本层源文件 + 编译/链接标志 |
| `Makefile` (层) | 读 Kconfig → 对象文件 → 追加到 ALL_OBJS |
| `Kconfig` (模块) | 声明本模块源文件 + 构建标志 |
| `Makefile` (模块) | 读 Kconfig → 对象文件 → 追加到 ALL_OBJS |

### 添加新模块

在模块层目录下创建子目录，放入 `Kconfig` + `Makefile` + 源文件即可。**不需要修改任何现有文件。** 层 Makefile 通过 `$(wildcard */Makefile)` 自动发现新模块。

## 十一、技术选型

| 项目 | 选择 | 理由 |
|------|------|------|
| 语言标准 | C11 + _GNU_SOURCE | 支持 _Atomic、stdatomic.h |
| 编译器 | GCC | 运行在 Linux 环境 |
| 构建系统 | Make | 轻量、零依赖 |
| 模块注册 | `__attribute__((constructor))` | 编译器自动处理 |
| 日志后端 | 无锁环形缓冲区 | 避免 mutex 死锁 |
| 事件总线 | pthread_mutex 保护 | 简单可靠 |
| 内存追踪 | 数组 + mutex | 简单高效 |
| LLM 协议 | OpenAI Chat Completions API | 归一化接口，多模型兼容 |
| 配置格式 | YAML | 人类可读、层次结构 |
| 技能格式 | Hermes SKILL.md (YAML+Markdown) | 兼容 Hermes 生态系统 |

---

## 十二、Agent 核心交互流程 (Application Layer)

### 12.1 REPL 循环 (`repl/agent_loop.c`)

```
stdin 输入 → agent_loop_repl()
  ├─ "exit"/"quit" → archive_flush_segment() + return
  ├─ 空行 → 提示帮助信息
  └─ 其他 → handle_ask(question, out, len)
              ├─ archive_detect_topic_shift(question)  # 话题漂移检测
              │   └─ 漂移 → archive_flush_segment()
              ├─ BUILD_PROMPT()                        # 构建三层 prompt
              ├─ Phase 1: llm_chat_stream()             # LLM API 调用
              │   └─ 指令解析(NOTE/PROFILE/SKILL/ARCHIVE/RECALL/SEMANTIC)
              ├─ archive_feed_turn(question, answer)    # 喂入 segment
              ├─ archive_append_log()                   # L5 归档
              └─ Phase 2 (如果技能被加载): 同上流程
```

### 12.2 三层 Prompt 架构 (`ask/ask.c` — BUILD_PROMPT 宏)

| 层 | 内容 | 大小 | 始终在 prompt? |
|----|------|------|---------------|
| L1: System | 身份 + Interface 指令列表 + 技能名列表 + 语言 + Topics 索引 | ~500B | 是 |
| L2: Context | 技能完整索引(含描述) | ~2KB | 仅 `[SKILL:list]` 时 |
| L3: Task | 技能完整正文 | ~10KB | 仅 `[SKILL:name]` 时 |

L1 的 Interface 指令清单:
```
[SKILL:<name>]     — 加载并执行技能
[SKILL:list]       — 列出所有技能
[NOTE:<fact>]      — 保存到工作记忆
[PROFILE:<info>]   — 保存用户画像
[FORGET:<key>]     — 从记忆删除
[ARCHIVE: topic=... | episode=... | importance=l|m|h|f]
                   — 归档事件 (关闭当前 segment, 开始新事件)
[RECALL:<keyword>] — 搜索已归档记忆
[SEMANTIC: knowledge=... | tags=...]
                   — 存储通用知识(不绑定事件)
```

### 12.3 LLM 指令处理 (Directive Loop)

LLM 响应中的指令按出现位置先后处理，每类指令的处理优先级由其文本位置决定(首次出现的指令最先被解析):

| 指令 | Kind | 处理逻辑 |
|------|------|---------|
| `[NOTE: text]` | 1 | 存入 memfile (memories/memory.md) |
| `[PROFILE: info]` | 2 | 存入用户画像 (memories/user.md) |
| `[FORGET: key]` | 3 | 从 memfile 删除匹配条目 |
| `[SKILL: name]` / `[SKILL:list]` | 4 | 加载技能内容 → 触发 Phase 2 LLM 调用 |
| `[ARCHIVE: ...]` | 5 | 委托 compat/directives.c 解析 key=value |
| `[RECALL: keyword]` | 6 | 搜索 L1+L2+L4，结果打印到 terminal |
| `[SEMANTIC: ...]` | 7 | 委托 compat/directives.c 解析 knowledge/tags |

### 12.4 语言自动检测

首次交互时检测用户输入是否包含 CJK 字符 (UTF-8 0xE4-0xE9)，自动设置 `Language=Chinese` 或 `Language=English`，存入用户画像。每轮注入语言指令到 prompt。

---

## 十三、Business 层 — 各模块详解

### 13.1 LLM Client (`llm_client/`)

**职责**: OpenAI Chat Completions API 的封装，支持流式输出。

**核心文件**:
- `llm_client.h/c` — 公共 API, 非流式和流式 chat completion
- `llm_core.h` — 核心 JSON 构建 + HTTP POST
- `model_deepseek.c` — DeepSeek 模型适配器 (streaming/polling)
- `model_agnes.c` — Agnes 模型适配器

**API 签名**:
```c
// 非流式
llm_response_t *llm_chat(const char *endpoint, const char *api_key,
                          const char *model, const char *prompt);
// 流式 (token callback)
llm_response_t *llm_chat_stream(const char *endpoint, const char *api_key,
                                 const char *model, const char *prompt,
                                 void (*on_token)(const char*, void*),
                                 void *user_data);
```

**模型适配器模式**: 每个模型实现 `model_io_t` 接口(init/deinit/stream/poll)。`llm_client.c` 通过 `LLM_MODEL_REGISTER` 宏自动发现可用的模型适配器，按 priority 排序后选择匹配的模型。

### 13.2 Skill Manager (`skill_manager/`)

**职责**: 扫描 `data/skills/<category>/<name>/SKILL.md`，解析 YAML frontmatter，建立可搜索的技能索引。

**核心 API**:
```c
int skill_scan(const char *dir);           // 扫描目录
int skill_index(char *buf, size_t len);    // 构建索引字符串
const char *skill_get_names_line(void);    // 紧凑技能名列表
char *skill_load(const char *name);        // 加载技能正文
```

**技能格式** (Hermes 兼容):
```markdown
---
name: novel-create
description: 小说正文创作
category: creative
---
<markdown body — LLM 遵循的指令>
```

**目录自动发现**: 在 `data/skills/` 下创建 `<category>/<name>/SKILL.md` 即可自动加载，无需注册。

### 13.3 Memory (`memory/`)

**职责**: 后端无关的记忆存储，支持 `note`/`recall`/`forget` 命令。

**后端抽象**:
```c
typedef struct memory_backend {
    int (*init)(void);
    int (*store)(const memory_entry_t *entry, char *id_out, size_t id_len);
    int (*retrieve)(const char *id, memory_entry_t *entry_out);
    int (*search)(const char *query, memory_entry_t *results,
                  int max_results, int *out_count);
    // ...
} memory_backend_t;
```

**默认后端**: TSV 文件 (`data/memory.db`)，每条记录: `id\ttype\ttimestamp\tcontent\tmetadata`

type 分类: `"note"` (普通记忆), `"conversation"` (对话), `"knowledge"` (知识), `"semantic"` (语义知识, 由 archive 模块使用), `"episode"` (情景, 预留)。

### 13.4 Tool Manager (`tool_manager/`)

**职责**: 工具注册与执行框架。用于未来 LLM function calling 支持。

```c
int tool_register(const tool_def_t *def);     // 注册工具
int tool_call(const char *name, const char *args_json,
              char *result, size_t result_len); // 执行工具
```

### 13.5 Archive — 分层记忆系统 (`archive/`)

**6 层人脑模拟记忆模型**:

| 层 | 类比 | 存储 | 大小 | 在 prompt? | 后端 |
|----|------|------|------|-----------|------|
| L0 | 当前意识 | `memories/memory.md` | ~2KB | 始终 | memfile (有界) |
| L1 | 记忆目录 | `memories/topics.md` | ≤50条 | 始终 | topics_file.c |
| L2 | 情景摘要 | `data/memory.db` type=episode | 按需 | 仅检索 | memory 模块 |
| L3 | 详细情景 | `data/events/{id}.json` | 按需 | 仅检索 | events.c |
| L4 | 语义知识 | `data/memory.db` type=semantic | 按需 | 仅检索 | semantic.c |
| L5 | 原始记录 | `data/archive/{date}/{session}.log` | 无限 | 无 | log_store.c |

**强度分公式** (L1 话题的活跃度):
```
score = (importance × 30 / 100)
      + recency_bonus(days)     // 30天内递减
      + recall_count × 4
      - days × 5
```
范围: 0-100。低于 20 从 L1 移除(索引级遗忘)。flash 标记永不剔除。

**事件分割** (conversation → events):
- **显式**: LLM 输出 `[ARCHIVE:]` 标记事件边界
- **隐式**: 每轮自动检测话题漂移 (keyword overlap 阈值 < 15%)
- **边界**: exit 时自动归档剩余对话

**四层架构** (模块内部):

```
ext/archive.h            ← External Interface (稳定公共 API, 外部模块只依赖这里)
    │
compat/archive.c         ← Compatibility Layer (协调映射, 每函数1行委托)
compat/directives.c      ← LLM 指令解析
    │
core/topics.c            ← Core Layer: L1 话题 CRUD + 缓存
core/strength.c          ← Core Layer: 强度分公式 (纯数学, 零 I/O)
core/segment.c           ← Core Layer: 事件分割
core/keywords.c          ← Core Layer: 关键词提取 (纯函数, 零依赖)
    │
exec/topics_file.c       ← Execution Layer: topics.md 文件 I/O
exec/events.c            ← Execution Layer: events/*.json
exec/log_store.c         ← Execution Layer: archive/*.log
exec/semantic.c          ← Execution Layer: memory.db 语义知识
```

**约束**: Core 层 `0次` os_file_open / memory.h。Exec 层 `0次` core 业务逻辑函数调用。

### 13.6 HTTP Client (`http_client/`)

**职责**: 基于 POSIX socket 的 HTTP(S) 客户端。支持 POST/GET，大响应分块读取。连接复用。OpenSSL 集成。

---

## 十四、配置文件 (config.yml)

```yaml
logging:
  file: agent.log
  level: info
threadpool:
  workers: 0                   # 0 = 自动(CPU核数)
llm:
  endpoint: https://.../v1
  model: agnes-2.0-flash
  api_key: "sk-..."
skills:
  dir: "data/skills"
debug:
  enabled: false               # 启用时输出完整 prompt 到 stderr
```

## 十五、当前实现状态

### 已完成

- **Platform**: os_api 完整接口 + POSIX 实现
- **Framework**: 模块注册(10 模块)、生命周期管理、事件总线、内存分配追踪、main() 入口
- **Foundation**: 无锁日志、线程池(CPU 自适应)、配置解析(YAML)
- **Business**: 
  - HTTP 客户端 (socket + SSL)
  - LLM 客户端 (流式 + 非流式, DeepSeek/Agnes 适配器)
  - Skill Manager (50 技能索引, 自动发现)
  - Tool Manager (工具注册框架)
  - Memory 模块 (后端抽象, TSV 文件实现)
  - Archive 模块 (6 层完整记忆系统, 四层架构内部分层)
- **Application**:
  - REPL 循环 (stdin 交互)
  - 三层 Prompt 构建 (System/Context/Task)
  - LLM 指令解析 (7 种指令类型)
  - 语言自动检测
  - 事件分割 (显式/隐式/边界)

### 待实现/可优化

- Archive L4 自动语义凝练 (当前仅简单的 topic→semantic 映射)
- Archive L2 Episode 的完整 search/recall 管道
- LLM function calling (Tool Manager 已就绪但未被 LLM 使用)
- 单元测试 (keywords.c 和 strength.c 可独立测试)
- 更多模型适配器
- 配置文件热重载
