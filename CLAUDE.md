# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make              # compile build/agent (C11, gcc)
make clean        # remove build/
build/agent       # interactive REPL
build/agent config set <key> <value>   # change config at runtime
build/agent config verify              # test LLM endpoint connectivity
python3 anthropic_proxy.py [port]      # Anthropic→OpenAI proxy for Claude Code
```

## Project Architecture

**Axon** — a modular C11 AI Agent framework (~9000 lines, 45+ .c/.h files). Five layers, strict downward-only calls:

| Layer | Dir | Priority | Responsibility |
|-------|-----|----------|----------------|
| Platform | `src/platform/` | 1-9 | OS API abstraction (file/socket/thread/mutex/env via `os_api.h`). POSIX impl; Windows stubs. |
| Framework | `src/framework/` | 100-299 | Module registry (ELF `.agent_modules` section), lifecycle (init→start→loop→stop→deinit), event bus, memory allocator tracking, monitor |
| Foundation | `src/foundation/` | 10-99 | Lock-free async logger (ring buffer+dedicated thread), YAML config parser, thread pool |
| Business | `src/business/` | 300-499 | LLM client (streaming + adapters), HTTP client (socket+SSL), skill manager (auto-scan `data/skills/`), tool manager (function-calling registry), memory backend (TSV file), archive (6-layer memory), agent tool loop |
| Application | `src/application/` | 500+ | REPL loop, prompt builder, command handlers (note/profile/forget/recall), bounded memfiles |

**Module registration**: Each module defines a `framework_module_t` struct and registers via `MODULE_REGISTER(mod)` macro which places a pointer into the `.agent_modules` ELF section. The linker script `agent.ld` collects these. Init order = Makefile link order (LAYERS variable in Kconfig).

**Build system**: Three-level Makefile hierarchy — `Makefile` → `layer/Makefile` → `module/Makefile`. Each reads Kconfig for source lists. Top-level `Kconfig` declares LAYERS. Adding a new module = create a subdirectory with Kconfig + Makefile + sources; no existing files need changes.

## Agent Loop

Two interacting loops:
1. **REPL (application layer)** — `src/application/agent_loop/repl/agent_loop.c`: stdin → command dispatch. Most inputs go through `handle_ask()` in `ask/ask.c`.
2. **Tool loop (business layer)** — `src/business/agent/agent.c`: `agent_run()` builds prompt → calls LLM → parses `<tool_call>...</tool_call>` → executes → repeats (max 6 rounds). Registered tools auto-described into prompt via `tool_schema_build()`.

`ask.c` is the bridge: it builds the 3-layer prompt (System/Context/Task), calls streaming LLM, parses 8 directive types ([NOTE], [PROFILE], [SKILL], [ARCHIVE], [RECALL], etc.), and delegates `<tool_call>` to the business-layer agent loop.

## 6-Layer Memory (Archive)

| Layer | Analogy | Storage | Location |
|-------|---------|---------|----------|
| L0 | Working memory | Bounded memfile | `memories/memory.md`, `memories/user.md` |
| L1 | Memory index | Topic list (≤50) | `memories/topics.md` |
| L2 | Episode summaries | memory.db type=episode | `data/memory.db` |
| L3 | Detailed events | JSON files | `data/events/{id}.json` |
| L4 | Semantic knowledge | memory.db type=semantic | `data/memory.db` |
| L5 | Raw logs | Archive files | `data/archive/{date}/{session}.log` |

Event segmentation: explicit (`[ARCHIVE:]` directive), implicit (keyword overlap < 15% = topic drift), and on-exit. Topics scored by importance×recency×recall-count formula.

## LLM Client

OpenAI Chat Completions API, streaming and non-streaming. Model adapter pattern (`model_io_t` interface via `LLM_MODEL_REGISTER` ELF section). Current adapters: DeepSeek (streaming/polling) and Agnes. Config via `config.yml`: endpoint, model, api_key, timeout.

## Tool System

Two coexisting systems:
- **`tool_manager`** (`src/business/tool_manager/`) — registry with JSON schema, risk levels (SAFE/WRITE/SHELL/DANGEROUS). Currently registered tools: `echo`, `list_dir`, `read_file`, `write_file`, `bash`.
- **`<tool_call>` format** (`src/business/agent/`) — XML-like tool invocation parsed by `tool_executor.c`. LLM writes `<tool_call>{"name":"...","arguments":{...}}</tool_call>`.

## Debug

Set `debug: enabled: true` in `config.yml` to dump full prompts to stderr.

## Key Patterns

- **No direct libc calls above Platform layer** — all system operations (file I/O, sockets, threads, string ops) go through `os_api.h` wrappers.
- **Prompt is flattened to one line** (newlines replaced with spaces) for JSON transport.
- **Language auto-detection**: first interaction checks for CJK UTF-8 lead bytes (0xE4–0xE9); stored in profile as `Language=Chinese` or `Language=English`.
- **Skills** are Hermes-compatible SKILL.md files in `data/skills/<category>/<name>/SKILL.md` with YAML frontmatter; auto-discovered at startup.
- **anthropic_proxy.py** is a standalone Flask proxy translating Anthropic API format → OpenAI API format for use with Claude Code CLI.
