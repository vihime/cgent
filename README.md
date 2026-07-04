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

Sessions are auto-saved to `~/.cgent/sessions/<uuid>/session.json` after each interaction. The session JSON contains the full conversation history, model config, and system prompt.

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
| `web_fetch` | Fetch content from a URL |
| `web_search` | Perform a web search |
| `send_message` | Send a message to the mailbox |
| `check_mailbox` | Check for unread mailbox messages |
| `clear_mailbox` | Clear mailbox messages |
| `spawn_subagent` | Spawn a child cgent process for parallel work |

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

Tests cover: JSON parsing, message lifecycle, config/AGENTS.md parsing, tool registry, tool execution (read_file, write_file, edit, bash, think, glob, grep), memory leak stress (1000+ iterations), mock HTTP backend, multi-turn chat, code generation, and subagent spawning.

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
