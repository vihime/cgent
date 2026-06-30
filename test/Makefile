# test/Makefile — cgent test suite

include ../config.mk
CFLAGS  = $(CFLAGS_BASE) -I../include -I../third_party/cJSON
LDFLAGS = $(LDFLAGS_BASE)

# Source files from main project (without main.c)
SRC_DIR  = ../src
PROJ_SRCS = $(SRC_DIR)/core/message.c \
            $(SRC_DIR)/core/streaming.c \
            $(SRC_DIR)/core/agent.c \
            $(SRC_DIR)/network/http_client.c \
            $(SRC_DIR)/network/sse_reader.c \
            $(SRC_DIR)/network/http_mock.c \
            $(SRC_DIR)/protocol/provider.c \
            $(SRC_DIR)/protocol/deepseek.c \
            $(SRC_DIR)/protocol/openai.c \
            $(SRC_DIR)/protocol/anthropic.c \
            $(SRC_DIR)/tools/tool_registry.c \
            $(SRC_DIR)/tools/tool_executor.c \
            $(SRC_DIR)/tools/builtin_tools.c \
            $(SRC_DIR)/subagent/subagent.c \
            $(SRC_DIR)/skills/skills.c \
            $(SRC_DIR)/config/args.c \
            $(SRC_DIR)/config/agent_md.c \
            $(SRC_DIR)/config/config.c \
            $(SRC_DIR)/json/json_wrapper.c \
            $(SRC_DIR)/platform/os.c \
            $(SRC_DIR)/platform/utf8_input.c \
            ../third_party/cJSON/cJSON.c

PROJ_OBJS = $(PROJ_SRCS:.c=.o)

# Test source files
TEST_SRCS = test_json.c test_message.c test_config.c test_tools.c \
            test_integration.c test_memory_leak.c test_subagent.c

# Test runner stub
TEST_RUNNER = runner.c

TEST_OBJS = $(TEST_RUNNER:.c=.o) $(PROJ_OBJS)

.PHONY: all clean test test-json test-message test-config test-tools \
        test-integration test-memory test-subagent test-all

all: test

# Build each test binary
test-json: $(PROJ_OBJS) test_json.c
	$(CC) $(CFLAGS) -o $@ test_json.c $(PROJ_OBJS) $(LDFLAGS)

test-message: $(PROJ_OBJS) test_message.c
	$(CC) $(CFLAGS) -o $@ test_message.c $(PROJ_OBJS) $(LDFLAGS)

test-config: $(PROJ_OBJS) test_config.c
	$(CC) $(CFLAGS) -o $@ test_config.c $(PROJ_OBJS) $(LDFLAGS)

test-tools: $(PROJ_OBJS) test_tools.c
	$(CC) $(CFLAGS) -o $@ test_tools.c $(PROJ_OBJS) $(LDFLAGS)

test-integration: $(PROJ_OBJS) test_integration.c
	$(CC) $(CFLAGS) -o $@ test_integration.c $(PROJ_OBJS) $(LDFLAGS)

test-memory: $(PROJ_OBJS) test_memory_leak.c
	$(CC) $(CFLAGS) -o $@ test_memory_leak.c $(PROJ_OBJS) $(LDFLAGS)

test-subagent: $(PROJ_OBJS) test_subagent.c
	$(CC) $(CFLAGS) -o $@ test_subagent.c $(PROJ_OBJS) $(LDFLAGS)

# Compile project objects
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

../third_party/cJSON/%.o: ../third_party/cJSON/%.c
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Run all tests
test: test-json test-message test-config test-tools test-memory
	@echo "=== Running JSON tests ==="
	@./test-json
	@echo ""
	@echo "=== Running Message tests ==="
	@./test-message
	@echo ""
	@echo "=== Running Config tests ==="
	@./test-config
	@echo ""
	@echo "=== Running Tool tests ==="
	@./test-tools
	@echo ""
	@echo "=== Running Memory leak tests ==="
	@./test-memory
	@echo ""
	@echo "=== All unit tests passed ==="

# Integration test (needs API key)
test-all: test test-integration
	@echo ""
	@echo "=== Running Integration tests ==="
	@./test-integration

clean:
	rm -f test-json test-message test-config test-tools test-integration test-memory test-subagent
	rm -f $(PROJ_OBJS)
	rm -f $(SRC_DIR)/**/*.d
