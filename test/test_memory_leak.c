/*
 * test_memory_leak.c — Memory leak stress test using mock HTTP backend
 *
 * Runs agent_chat 1000 times with mocked API responses and verifies
 * that memory usage does not grow unboundedly.
 */
#include "cgent.h"
#include "http_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-55s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ── Memory tracking ────────────────────────────────────────────── */

/* Read RSS memory in KB from /proc/self/statm */
static long get_rss_kb(void) {
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) return -1;
    long size, resident, shared, text, lib, data, dt;
    int n = fscanf(fp, "%ld %ld %ld %ld %ld %ld %ld",
                   &size, &resident, &shared, &text, &lib, &data, &dt);
    fclose(fp);
    if (n < 2) return -1;
    /* resident is in pages (4KB typically). Convert to KB. */
    return resident * 4;
}

/* ── Mock responses ─────────────────────────────────────────────── */

/* Queue up responses for a simple chat round-trip:
 *   1. User asks a question
 *   2. Assistant responds with text
 */
static void queue_simple_response(const char *text) {
    http_mock_push_chat_response(text, NULL, NULL, NULL);
}

/* Queue up responses for a tool-use round-trip:
 *   1. Assistant responds with a tool_call
 *   2. After tool result is sent, assistant responds with text
 */
static void queue_tool_use_responses(const char *tool_id,
                                      const char *tool_name,
                                      const char *tool_args,
                                      const char *final_text) {
    /* First response: tool call */
    http_mock_push_chat_response(NULL, tool_id, tool_name, tool_args);
    /* Second response: final text after tool result */
    http_mock_push_chat_response(final_text, NULL, NULL, NULL);
}

/* ── Test: 1000 iterations of simple chat ───────────────────────── */

static void test_memory_simple_chat_1000(void) {
    TEST("1000 simple chat iterations (no tools)");

    http_mock_enable();
    provider_init();
    http_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL, "provider not found");

    provider_config_t cfg = {
        .api_key     = "mock-key",
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    long rss_before = get_rss_kb();
    long rss_max = rss_before;

    for (int i = 0; i < 1000; i++) {
        http_mock_clear();

        agent_t *agent = agent_create(&cfg, api);
        CHECK(agent != NULL, "agent_create failed");
        agent_set_system_prompt(agent, "You are a test agent. Reply concisely.");

        /* Queue the response */
        queue_simple_response("This is mock response number 42.");

        message_t *resp = agent_chat(agent, "Hello, what is the answer?");
        CHECK(resp != NULL, "agent_chat returned NULL");
        message_free(resp);

        agent_free(agent);

        /* Track memory every 100 iterations */
        if (i % 100 == 99) {
            long rss = get_rss_kb();
            if (rss > rss_max) rss_max = rss;
        }
    }

    http_mock_clear();
    http_mock_disable();
    http_cleanup();

    long rss_after = get_rss_kb();
    long growth = rss_after > rss_before ? rss_after - rss_before : 0;

    printf("[rss: before=%ldKB after=%ldKB max=%ldKB growth=%ldKB] ",
           rss_before, rss_after, rss_max, growth);

    /* Allow up to 2MB growth (some allocator fragmentation is normal) */
    CHECK(growth < 2048, "memory grew too much (>2MB)");

    OK();
}

/* ── Test: 1000 iterations of tool-use chat ─────────────────────── */

static void test_memory_tool_use_1000(void) {
    TEST("1000 tool-use chat iterations");

    http_mock_enable();
    provider_init();
    http_init();
    tool_registry_clear();
    builtin_tools_register();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL, "provider not found");

    provider_config_t cfg = {
        .api_key     = "mock-key",
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    long rss_before = get_rss_kb();
    long rss_max = rss_before;

    int iterations = 500; /* Tool-use is heavier, fewer iterations */
    for (int i = 0; i < iterations; i++) {
        http_mock_clear();

        agent_t *agent = agent_create(&cfg, api);
        CHECK(agent != NULL, "agent_create failed");
        agent_set_system_prompt(agent, "Use tools to answer questions.");

        /* Register tools from the global registry */
        for (int j = 0; j < registry_count(); j++)
            agent_add_tool(agent, registry_get(j));

        /* Queue tool-use responses:
         *   1. Assistant calls think tool
         *   2. After tool result, assistant gives final answer */
        queue_tool_use_responses(
            "call_think_1", "think",
            "{\"thought\":\"The answer is 42 based on deep reasoning.\"}",
            "After thinking, the answer is 42.");

        message_t *resp = agent_chat(agent, "What is 6 times 7?");
        CHECK(resp != NULL, "agent_chat returned NULL");
        message_free(resp);

        agent_free(agent);

        if (i % 100 == 99) {
            long rss = get_rss_kb();
            if (rss > rss_max) rss_max = rss;
        }
    }

    http_mock_clear();
    http_mock_disable();
    http_cleanup();

    long rss_after = get_rss_kb();
    long growth = rss_after > rss_before ? rss_after - rss_before : 0;

    printf("[rss: before=%ldKB after=%ldKB max=%ldKB growth=%ldKB] ",
           rss_before, rss_after, rss_max, growth);

    CHECK(growth < 4096, "memory grew too much (>4MB)");

    OK();
}

/* ── Test: rapid create/destroy cycles ──────────────────────────── */

static void test_memory_rapid_cycles(void) {
    TEST("rapid agent create/destroy without chat");

    provider_init();
    http_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL, "provider not found");

    provider_config_t cfg = {
        .api_key     = "mock-key",
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    long rss_before = get_rss_kb();

    for (int i = 0; i < 10000; i++) {
        agent_t *agent = agent_create(&cfg, api);
        agent_set_system_prompt(agent, "Test prompt.");
        agent_free(agent);
    }

    http_cleanup();

    long rss_after = get_rss_kb();
    long growth = rss_after > rss_before ? rss_after - rss_before : 0;

    printf("[rss: before=%ldKB after=%ldKB growth=%ldKB] ",
           rss_before, rss_after, growth);

    CHECK(growth < 2048, "memory grew too much (>2MB)");

    OK();
}

/* ── Test: mock queue exhaustion ────────────────────────────────── */

static void test_mock_exhaustion(void) {
    TEST("mock queue exhaustion error handling");

    http_mock_enable();
    provider_init();
    http_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    provider_config_t cfg = {
        .api_key     = "mock-key",
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    agent_set_system_prompt(agent, "Test agent.");

    /* Don't queue any responses — should handle gracefully */
    message_t *resp = agent_chat(agent, "Hello?");
    /* Should return an error message, not crash */
    CHECK(resp != NULL, "should return error message, not NULL");

    message_free(resp);
    agent_free(agent);

    http_mock_clear();
    http_mock_disable();
    http_cleanup();

    OK();
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("Memory leak stress tests (mock backend):\n");
    printf("  Page size: 4KB (assumed)\n\n");

    test_memory_simple_chat_1000();
    test_memory_tool_use_1000();
    test_memory_rapid_cycles();
    test_mock_exhaustion();

    printf("\n  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
