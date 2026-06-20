# Axon Agent API & Architecture Reference

## Project Structure

```
src/
  platform/     # OS abstraction (file,socket,thread,etc.)
  framework/    # Module registry, lifecycle, event bus
  foundation/   # Logger, config parser, thread pool, token counter
  business/     # LLM client, HTTP client, tool system, agent loop, skill mgr
  application/  # REPL, input, memory, command handlers
data/
  tools/        # Dynamic tool JSON configs (P3-11)
  skills/       # Hermes-compatible SKILL.md files
  memory/l0/    # Persistent L0 memory files
```

## Configuration (config.yml)

```yaml
logging:
  file: agent.log       # log file path (empty = stderr only)
  level: info           # debug|info|warn|error|fatal

llm:
  endpoint: https://... # OpenAI-compatible API endpoint
  model: agnes-2.0-flash
  api_key: "sk-..."
  timeout: 30           # HTTP timeout in seconds

tools:
  shell_confirm: false  # true=prompt before bash, false=auto-run

debug:
  enabled: true         # enable verbose stderr debug logs
```

## Architecture Layers

| Layer | Priority | Module | Description |
|-------|----------|--------|-------------|
| Platform | 1-9 | os_api | File/socket/thread/mutex/env wrappers |
| Framework | 100-299 | registry | ELF .agent_modules section discovery |
| | | lifecycle | Module init→start→loop→stop→deinit |
| | | bus | Event pub/sub |
| Foundation | 10-99 | config | YAML config parser |
| | | logger | Lock-free async ring-buffer logger |
| | | threadpool | Worker thread pool |
| | | token_counter | Prompt token estimator |
| Business | 300-499 | llm_client | LLM API client (streaming+non-streaming) |
| | | http_client | HTTP/HTTPS via curl |
| | | agent | Tool call loop (max_depth=4) |
| | | tool_manager | Tool registry + 5 built-in tools |
| | | tool_executor | Parse+validate+execute tool calls |
| | | tool_schema | Auto-generate tool descriptions for prompt |
| | | skill_manager | Skill loader |
| Application | 500+ | agent_loop | REPL loop, command dispatch |
| | | ask | Prompt builder, LLM interaction, streaming token display |
| | | input | Raw-mode terminal input (history/cursor/UTF-8) |
| | | memfile | Bounded memory file (dedup on load/add) |
| | | handlers | note/profile/forget/recall/echo/reload commands |

## Tools API

### Built-in Tools

| Tool | Risk | Description |
|------|------|-------------|
| echo | SAFE | Echo back input arguments |
| list_dir | SAFE | List directory contents |
| read_file | SAFE | Read file content |
| write_file | WRITE | Write content to file |
| bash | SHELL | Execute shell command (with 30s timeout) |

### Tool Call Format

```
<tool_call>
{"name":"bash","arguments":{"command":"uname -r"}}
</tool_call>
```

LLM should use single quotes to avoid JSON escaping issues.

### Dynamic Tools

Place JSON configs in `data/tools/*.json`:

```json
{
  "name": "my_tool",
  "description": "What this tool does",
  "risk": "safe",
  "params": {
    "type": "object",
    "properties": {
      "arg1": {"type": "string", "description": "first argument"}
    }
  }
}
```

## Memory System

### L0 (Working Memory)
- File: `data/memory/l0/working.md`
- Format: entries separated by `\n§\n`
- Auto-dedup on load and add

### REPL Commands

| Command | Description |
|---------|-------------|
| `ask <question>` | Send question to LLM |
| `note <text>` | Save to working memory |
| `profile <key=value>` | Save user profile |
| `forget <keyword>` | Remove from memory |
| `recall <keyword>` | Search memories |
| `reload` | Hot-reload config.yml |
| `help` | Show help |
| `echo <msg>` | Echo test |

## Build

```bash
make              # compile build/agent
make clean        # remove build/
bash test.sh      # run test suite
bash test.sh tools # run tool tests only
```

## Error Codes

| Return | Meaning |
|--------|---------|
| 0 | Success (normal answer or tool completed) |
| 1 | Max depth reached (used all 4 tool rounds) |
| -1 | Error (LLM unavailable, parse failure, etc.) |
