# cgent — Pure C AI Agent
# Zero external dependencies (only system OpenSSL + libc)

include config.mk

# ── Source files ──────────────────────────────────────────────────

CORE_SRCS   = src/core/agent.c src/core/message.c src/core/streaming.c
NET_SRCS    = src/network/http_client.c src/network/sse_reader.c \
              src/network/http_mock.c
PROTO_SRCS  = src/protocol/provider.c src/protocol/deepseek.c \
              src/protocol/openai.c src/protocol/anthropic.c
TOOL_SRCS   = src/tools/tool_registry.c src/tools/tool_executor.c \
              src/tools/builtin_tools.c
SKILL_SRCS  = src/skills/skills.c
SUBAGENT_SRCS = src/subagent/subagent.c
CONFIG_SRCS = src/config/args.c src/config/agent_md.c src/config/config.c
JSON_SRCS   = src/json/json_wrapper.c
PLAT_SRCS   = src/platform/os.c src/platform/utf8_input.c

THIRD_SRCS  = third_party/cJSON/cJSON.c

ALL_SRCS    = src/main.c \
              $(CORE_SRCS) $(NET_SRCS) $(PROTO_SRCS) $(TOOL_SRCS) \
              $(SKILL_SRCS) $(SUBAGENT_SRCS) \
              $(CONFIG_SRCS) $(JSON_SRCS) $(PLAT_SRCS) \
              $(THIRD_SRCS)

ALL_OBJS    = $(ALL_SRCS:.c=.o)
ALL_DEPS    = $(ALL_SRCS:.c=.d)

TARGET      = cgent

# ── Targets ───────────────────────────────────────────────────────

.PHONY: all clean static install tags format help test

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	@echo "  LD    $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile with dependency generation
%.o: %.c
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Include generated dependencies
-include $(ALL_DEPS)

# Smallest possible binary (LTO + gc-sections + strip)
# Requires clean rebuild for LTO; run: make clean && make small
small: CFLAGS  += $(CFLAGS_SMALL)
small: LDFLAGS += $(LDFLAGS_SMALL)
small: $(ALL_OBJS)
	@echo "  LD    $(TARGET)-small"
	$(Q)$(CC) $(CFLAGS) -o $(TARGET)-small $^ $(LDFLAGS)
	@echo "  Size:"
	@ls -lh $(TARGET)-small | awk '{print "  " $$5 " " $$9}'

# Static build (no shared library deps)
static: CFLAGS  += -static
static: LDFLAGS += -static -lz -lzstd
static: $(ALL_OBJS)
	@echo "  LD    $(TARGET)-static"
	$(Q)$(CC) $(CFLAGS) -o $(TARGET)-static $^ $(LDFLAGS)

# Clean everything
clean:
	@echo "  CLEAN"
	$(Q)rm -f $(ALL_OBJS) $(ALL_DEPS) $(TARGET) $(TARGET)-static
	$(Q)rm -f compile_commands.json

# Install to system
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

# Build module object files only (for incremental dev)
modules: $(filter-out src/main.o,$(ALL_OBJS))

# Generate compile_commands.json for LSP (clangd)
compile_commands.json: clean
	$(Q)bear -- make 2>/dev/null || \
		(echo "Install 'bear' for compile_commands.json" && exit 0)

# Run tests
test:
	$(MAKE) -C test test

# Tags for editor navigation
tags:
	ctags -R --c-kinds=+p --fields=+iaS --extra=+q include/ src/

# Format C source
format:
	clang-format -i include/*.h src/**/*.c src/*.c 2>/dev/null || true

help:
	@echo "cgent build targets:"
	@echo "  make          — build cgent"
	@echo "  make static   — build statically linked cgent"
	@echo "  make clean    — remove build artifacts"
	@echo "  make install  — install to \$$PREFIX/bin"
	@echo "  make test     — run tests"
	@echo "  make tags     — generate ctags"
	@echo "  make help     — this message"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1       — debug build with sanitizers"
	@echo "  V=1           — verbose output"
	@echo "  PREFIX=/usr   — install prefix"
