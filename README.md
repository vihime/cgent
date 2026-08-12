# cgent — Pure C AI Agent

A multi-provider AI agent written entirely in C11. Zero external library dependencies — only system OpenSSL and libc are required.

Supports Anthropic, OpenAI, and DeepSeek APIs with tools, subagents, real-time SSE streaming, session persistence, and an interactive REPL.

## Quick Start

```bash
# Build & install
make && make install

# Configure API key
export CGENT_API_KEY="sk-your-key"            # env var

# Run
cgent -q "What is 2+2?"                        # single query
cgent -q "Read /etc/hostname"                  # with tool use
cgent                                           # interactive REPL
cgent --resume <uuid>                           # resume a session
cgent mcp list                                  # manage MCP servers
cgent skill list                                # manage skills
```

## Requirements

- **Linux** (primary target), macOS/Windows via MinGW (planned)
- **gcc 11+** or clang with C11 support
- **OpenSSL** (libssl-dev, libcrypto-dev) — for HTTPS
- **curl** (recommended) — for `web_fetch` and `web_search` tools
- **make** — build system

```bash
# Ubuntu/Debian
sudo apt install build-essential libssl-dev curl

# macOS
brew install openssl curl
```

## Configuration

### `~/.cgent/settings.json`

```json
{
  "default_model": "deepseek-chat",
  "agent_dir": "agents/cgent/",
  "models": {
    "deepseek-chat": {
      "provider": "deepseek",
      "api_key": "sk-your-key",
      "base_url": "https://api.deepseek.com",
      "temperature": 0.7,
      "max_tokens": 4096,
      "stream": true,
      "context_length": 1000000
    },
    "deepseek-reasoner": {
      "provider": "deepseek",
      "api_key": "sk-your-key",
      "base_url": "https://api.deepseek.com",
      "temperature": 0.0,
      "max_tokens": 8192,
      "stream": true,
      "thinking": {"type": "enabled"},
      "reasoning_effort": "high"
    },
    "gpt-4o": {
      "provider": "openai",
      "api_key": "",
      "base_url": "https://api.openai.com",
      "temperature": 0.7,
      "max_tokens": 4096,
      "stream": true
    }
  }
}
```

Each model has independent `api_key`, `base_url`, `temperature`, `max_tokens`, and `stream` settings.

### Configuration Priority (lowest → highest)

1. Built-in defaults
2. `~/.cgent/settings.json`
3. Environment variables (`CGENT_API_KEY`)
4. `AGENTS.md` from agent directory
5. CLI arguments (`--model`, `--api-key`, etc.)

### Agent Configuration (`AGENTS.md`)

```
agents/
  cgent/
    AGENTS.md          ← Default agent
  myagent/
    AGENTS.md          ← Use with: cgent -a agents/myagent
```

`AGENTS.md` is a Markdown file with YAML frontmatter:

```markdown
---
name: my-agent
description: A custom agent
model: gpt-4o
skills:
  - code-review
---
You are a helpful assistant. Follow these rules...
```

The body text after `---` becomes the system prompt.

### Skills Directory

Skills are loaded from `~/.cgent/skills/`, each skill is a subdirectory:

```
~/.cgent/skills/
  code-review/
    SKILL.md        # Skill definition (YAML frontmatter + instructions)
    scripts/        # Optional helper scripts
```

Skills become REPL commands (e.g., `/code-review`) and appear in `/skills`.

### Session Management

Sessions are auto-saved to `~/.cgent/sessions/<uuid>/session.jsonl` after each interaction. The JSONL file contains one JSON object per line: a metadata header line followed by one line per message with the full conversation history, raw API responses, model config, and system prompt.

```
Session: f8ee8a4c-11ff-4f3b-8f17-b7e1dcd4d439
> Hello
...
Resume: cgent --resume f8ee8a4c-11ff-4f3b-8f17-b7e1dcd4d439
```

The resume command is printed on exit for easy copy-paste.

### Deep Thinking / Reasoning

Configure chain-of-thought reasoning per model:

```json
"deepseek-reasoner": {
  "thinking": {"type": "enabled"},
  "reasoning_effort": "high"
}
```

Also supports `"output_config": {"effort": "high"}` format.

## CLI Reference

```
Usage: ./cgent [OPTIONS]

Options:
  -p, --provider <name>    API provider: deepseek, openai, anthropic
                           (default: deepseek)
  -m, --model <name>       Model name (default: deepseek-chat)
  -k, --api-key <key>      API key override for current provider
  -u, --base-url <url>     Override API base URL
  -q, --query <text>       Single query mode (non-interactive)
  -a, --agent <dir>        Agent directory (default: agents/cgent/)
  -t, --temperature <t>    Temperature 0.0-2.0 (default: 0.7)
  -M, --max-tokens <n>     Max output tokens (default: 4096)
  -n, --no-stream          Disable streaming output
  -c, --config <path>      Config file path
  -r, --resume <uuid>      Resume a saved session
  -v, --verbose            Verbose/debug output
  -h, --help               Show this help
  -V, --version            Show version

Environment:
  CGENT_API_KEY            API key for all providers
  CGENT_MODEL              Default model
  CGENT_PROVIDER           Default provider
  CGENT_AGENT_DIR          Agent directory path

Examples:
  ./cgent -q "What is 2+2?"
  ./cgent -p openai -m gpt-4o -q "Explain C pointers"
  ./cgent -a agents/myagent -q "Hello"
  ./cgent                   # starts interactive REPL
```

## REPL Commands

| Command | Description |
|---|---|
| `/help` | Show help |
| `/quit`, `/exit` | Exit REPL |
| `/clear` | Clear conversation history |
| `/tools` | List available tools |
| `/agents` | List installed agents (`*` = active) |
| `/context` | Show context usage breakdown |
| `/model` | List available models (`*` = active) |
| `/model <name>` | Switch to a different model |
| `/skills` | List loaded skills |
| `!<cmd>` | Execute a bash command (`!env`, `!ls`) |
| **Tab** | Auto-complete slash commands |
| **Up/Down arrows** | Navigate input history |
| **Left/Right arrows** | Move cursor within input |
| **Ctrl-D** | Exit (on empty line) |
| **Ctrl-C** | Cancel current input |

## Built-in Tools

| Tool | Description |
|---|---|
| `read_file` | Read a file from disk |
| `write_file` | Write content to a file |
| `edit` | Exact string replacement in a file |
| `bash` | Execute a shell command |
| `think` | Record a thought (chain-of-thought) |
| `glob` | Find files matching a glob pattern |
| `grep` | Search for text patterns in files |
| `list_dir` | List directory entries with type and size |
| `apply_patch` | Apply a unified diff patch (add/edit/delete files) |
| `git_status` | Show git branch and changed files |
| `git_diff` | Show git diff (staged, stat, or path-filtered) |
| `git_log` | Show recent git commit history |
| `todo_write` | Replace the agent's todo/plan list |
| `todo_update` | Update one todo item's status or text |
| `todo_list` | Show the current todo list |
| `web_fetch` | Fetch content from a URL |
| `web_search` | Perform a web search |
| `send_message` | Send a message to the mailbox |
| `check_mailbox` | Check for unread mailbox messages |
| `clear_mailbox` | Clear mailbox messages |
| `spawn_subagent` | Spawn a child cgent process for parallel work |

### Subagent Enhancements

Beyond the blocking `spawn_subagent` tool, cgent exposes an async subagent
handle API for orchestration:

- `subagent_spawn()` — start a child without blocking
- `subagent_poll()` — process progress logs and follow-up answers
  (delivered via an `on_event` callback)
- `subagent_followup()` — send a new instruction mid-task; the child
  continues its conversation and replies with an `update` event
- `subagent_stop()` / `subagent_abort()` — graceful stop after the current
  turn, or an immediate kill
- `subagent_wait()` — block for the final result

The child keeps its conversation state across follow-up turns, so a
subtask can be steered iteratively without restarting. The synchronous
`subagent_run()` still works unchanged.

Plain `http://` base URLs are now supported by the HTTP client (useful for
local LLM servers such as Ollama or vLLM), which the follow-up test uses.

## MCP Server Management

cgent can manage [MCP](https://modelcontextprotocol.io) (Model Context
Protocol) servers via the `mcp` subcommand. Servers are stored in
`~/.cgent/mcp.json`.

```bash
# List configured servers
cgent mcp list

# Add a server (command + args + optional env/cwd)
cgent mcp add filesystem \
  --command npx \
  --args "-y,@modelcontextprotocol/server-filesystem,/tmp" \
  --env "MY_TOKEN=abc123" \
  --cwd /home/user

# Verify a server: spawns it and performs the MCP initialize handshake,
# then lists its tools
cgent mcp test filesystem

# Remove a server
cgent mcp remove filesystem
```

`cgent mcp test` talks to the server over stdio JSON-RPC (the standard MCP
stdio transport) and reports the server name/version, protocol version, and
the tools it exposes.

### Using MCP Tools in Conversations

Configured MCP servers can be connected at runtime — their tools are
discovered via the `tools/list` handshake and exposed to the model as
`<server>__<tool>` (e.g. `filesystem__read_file`). When the model calls
one, cgent forwards the invocation to the server via `tools/call` and
returns the result as a normal tool result.

```bash
# Enable specific servers (repeatable)
cgent --mcp filesystem --mcp database

# Enable every configured server
cgent --mcp-all
```

Alternatively, list servers in the agent's `AGENTS.md` to enable them
automatically for that agent:

```markdown
---
mcp_servers:
  - filesystem
---
```

Servers that fail to start (or expose no tools) are reported as warnings
on stderr and do not block the session.

## Reliability & Usage

Transient API failures (429, 5xx, or network errors) are retried with
exponential backoff and jitter before giving up. Configure the retry
count per model in `settings.json` (`"max_retries": 3`), with `--retries`
on the CLI, or switch it at runtime with `/model`.

Streaming requests ask the API for usage data (`stream_options.include_usage`)
and both streaming and non-streaming responses record real
`prompt_tokens`/`completion_tokens`. Per-session totals are shown after
each turn on stderr, persisted in the session file, and inspectable in
the REPL with `/usage` or `/context`:

```
[usage] 2 request(s), 1234 in / 567 out tokens (total 1801)
```

### Automatic Context Management

Before each request, cgent estimates the conversation size (ASCII ~4
chars/token, CJK ~1 char/token — a much better fit for Chinese than the
old flat heuristic). When the estimate exceeds `compact_ratio` of the
model's context window, the conversation is automatically summarized via
a compaction request and replaced with the summary. If compaction fails
or the summary is still too large, the oldest messages are trimmed.

Configure per model in `settings.json`:

```json
{
  "models": {
    "deepseek-v4-flash": {
      "auto_compact": true,
      "compact_ratio": 0.75
    }
  }
}
```

Disable at runtime with `--no-auto-compact`. `/context` now uses the same
CJK-aware estimator.

### Tool Approval

Risky built-in tools (`bash`, `write_file`, `edit`, `apply_patch`) prompt
for confirmation before executing in the interactive REPL. The `confirm`
tool lets the model explicitly ask the user for permission before a
destructive action.

```text
[cgent] Tool 'bash' requires your approval.
  args: {"command":"rm -rf build"}

Approve? [y/N] n
```

Single-shot `-q` mode and `--yes` skip the prompts (the command is
explicitly user-initiated), and subagent processes are non-interactive.

### Parallel Tool Execution

When the model returns multiple tool calls in one response, cgent runs
independent tools in parallel (up to 32 threads) instead of one at a
time. Tools that share mutable state — the `confirm` tool (stdin) and
MCP tools (one stdio pipe per server session) — stay sequential. Tool
results are appended in call order regardless of completion order.

Configure per model in `settings.json` (`"parallel_tools": true`, the
default) or disable at runtime with `--no-parallel-tools`.

### Graceful Ctrl-C Cancellation

`Ctrl-C` no longer kills cgent. A SIGINT handler sets a cancellation flag
that the request loops check:

- During streaming, the current generation is aborted (partial output is
  discarded) and the REPL returns to the prompt — the session stays
  usable.
- During command execution, the running command is killed and the tool
  reports `(command interrupted)`.
- At the input prompt, `Ctrl-C` clears the current line.
- In single-shot `-q` mode, the request is cancelled and cgent exits
  with status 130.

### Structured Output

Request JSON-shaped responses instead of free text:

```bash
# JSON object mode (DeepSeek/OpenAI)
cgent --json -q "List 3 project ideas as JSON"

# JSON schema mode — enforce a specific schema from a file
cgent --json-schema schema.json -q "Describe the API"
```

The schema is passed through as `response_format` in the request. In the
REPL, `/json` toggles JSON object mode. When a structured format is
active, cgent strips ```` ```json ```` code fences from the output so the
result is clean JSON. Per-model defaults can be set in `settings.json`
with `"response_format"` and `"json_schema"`.

### Task Planning (Todos)

For multi-step tasks the agent can lay out an explicit plan with
`todo_write` (replacing the whole list — the model owns the plan), then
mark progress with `todo_update` and review with `todo_list`. Items carry
a status (`pending`, `in_progress`, `completed`, `cancelled`). The list is
persisted in the session file, so it survives `--resume`, and the REPL
shows it with `/todos`.

## Skill Management

Skills are markdown definitions in `~/.cgent/skills/<name>/SKILL.md` with
YAML frontmatter (`name`, `description`) and an instruction body. They are
loaded at startup, listed in the system prompt, and invocable in the REPL
with `/name`.

```bash
# List installed skills
cgent skill list

# Create a skill
cgent skill add code-review \
  --description "Review code for bugs and style issues" \
  --instruction "Review the given code carefully for bugs, security, and style issues."

# Show a skill's full definition
cgent skill show code-review

# Delete a skill (use --force on add to overwrite an existing one)
cgent skill remove code-review
```

## Architecture

```
cgent/
├── include/           # Public headers
│   ├── cgent.h        # Umbrella header
│   ├── core.h         # Agent, message, tool structs
│   ├── config.h       # Configuration & CLI args
│   ├── network.h      # HTTP client, SSE reader
│   ├── protocol.h     # API provider abstraction
│   ├── tools.h        # Tool registry & execution
│   ├── subagent.h     # Subagent spawning API
│   ├── session.h      # Session persistence
│   ├── skills.h       # Skill loading
│   ├── http_mock.h    # Mock HTTP backend (testing)
│   ├── json.h         # JSON wrapper (cJSON)
│   └── platform.h     # OS abstraction & terminal I/O
├── src/
│   ├── main.c         # Entry point, CLI, REPL
│   ├── core/          # agent.c, message.c, streaming.c
│   ├── network/       # http_client.c, sse_reader.c, http_mock.c
│   ├── protocol/      # provider.c, deepseek.c, openai.c, anthropic.c
│   ├── tools/         # tool_registry.c, tool_executor.c, builtin_tools.c
│   ├── subagent/      # subagent.c (fork+exec with JSON IPC)
│   ├── skills/        # skills.c (load from ~/.cgent/skills/)
│   ├── session/       # session.c (save/restore to ~/.cgent/sessions/)
│   ├── config/        # config.c, args.c, agent_md.c
│   ├── json/          # json_wrapper.c
│   └── platform/      # os.c, utf8_input.c
├── agents/            # Agent configurations (AGENTS.md files)
├── test/              # Test suite (34 tests)
├── third_party/cJSON/ # Embedded JSON library (MIT)
├── Makefile           # Build system
├── config.mk          # Platform detection & flags
└── settings.json.example
```

### Data Flow

```
User input → CLI/REPL → Agent Core → Protocol → HTTP/TLS → API
                                      ↑                      │
                                      │   Tool System ←──────┘
                                      │   (read_file, edit, etc.)
                                      ↓
                         SSE streaming (real-time tokens)
                         Subagent (fork+exec, IPC)
                         Session (auto-save to ~/.cgent/sessions/)
```

### Zero Dependencies

The HTTP client is implemented directly over raw OpenSSL sockets — no libcurl for API calls. Real-time SSE streaming with incremental chunked reads. The JSON parser is cJSON embedded as a single `.c`/`.h` pair (MIT license). Everything else is built from scratch in C11.

Binary size: **95K** default (`-Os -s`), **79K** with `make small` (LTO + gc-sections).

## Build Targets

```bash
make              # Build cgent (95K, -Os -s)
make small        # Smallest binary (79K, LTO + gc-sections)
make install      # Install to ~/.local/bin (default)
make static       # Static binary (needs libzstd-static)
make test         # Run all unit tests (25 tests)
make clean        # Remove build artifacts
```

## Test Suite

```bash
make test                                      # 22 unit tests
make -C test test-integration                  # 8 integration tests (needs API key)
CGENT_API_KEY=sk-xxx make -C test test-subagent     # 3 subagent tests
```

Tests cover: JSON parsing, message lifecycle, config/AGENTS.md parsing, tool registry, tool execution (read_file, write_file, edit, bash, think, glob, grep, list_dir, apply_patch, git_status, git_diff, git_log), exec timeout/truncation, shell-argument escaping, memory leak stress (1000+ iterations), mock HTTP backend, multi-turn chat, code generation, and subagent spawning.

## Contributing

Build with `DEBUG=1` for sanitizers:

```bash
make clean && DEBUG=1 make
```

Format code:

```bash
clang-format -i include/*.h src/**/*.c test/*.c
```

## License

MIT
