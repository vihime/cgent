# cgent — Pure C AI Agent

A multi-provider AI agent written entirely in C11. Zero external library dependencies — only system OpenSSL and libc are required.

Supports Anthropic, OpenAI, and DeepSeek APIs with tools, subagents, streaming, and an interactive REPL.

## Quick Start

```bash
# Build
make

# Configure API key (choose one method)
export DEEPSEEK_API_KEY="sk-your-key"        # env var
# or edit ~/.cgent/settings.json → deepseek-chat.api_key

# Run
./cgent -q "What is 2+2?"                     # single query
./cgent -q "Read /etc/hostname"               # with tool use
./cgent                                        # interactive REPL
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
      "stream": true
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
3. Environment variables (`DEEPSEEK_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`)
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
  -v, --verbose            Verbose/debug output
  -h, --help               Show this help
  -V, --version            Show version

Environment:
  DEEPSEEK_API_KEY         DeepSeek API key
  OPENAI_API_KEY           OpenAI API key
  ANTHROPIC_API_KEY        Anthropic API key
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
| `/model` | List available models (`*` = active) |
| `/model <name>` | Switch to a different model |
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
                                      │   (read_file, bash, etc.)
                                      ↓
                                  Subagent (fork+exec, IPC)
```

### Zero Dependencies

The HTTP client is implemented directly over raw OpenSSL sockets — no libcurl, no libuv. The JSON parser is cJSON embedded as a single `.c`/`.h` pair (MIT license). Everything else is built from scratch in C11.

Binary size: **95K** default (`-Os -s`), **79K** with `make small` (LTO + gc-sections).

## Build Targets

```bash
make              # Build cgent (95K, -Os -s)
make small        # Smallest binary (79K, LTO + gc-sections)
make static       # Static binary (needs libzstd-static)
make test         # Run all unit tests (25 tests)
make clean        # Remove build artifacts
make install      # Install to $PREFIX/bin
```

## Test Suite

```bash
make test                                      # 22 unit tests
make -C test test-integration                  # 8 integration tests (needs API key)
DEEPSEEK_API_KEY=sk-xxx make -C test test-subagent  # 3 subagent tests
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
