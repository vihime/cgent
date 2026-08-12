/*
 * test_agent_reliability.c — Retry/backoff and usage accounting tests
 *
 * Uses the mock HTTP backend to inject 5xx responses followed by
 * successful chat completions, and verifies that the agent retries
 * transient failures and records API usage tokens.
 */
#include "cgent.h"
#include "core.h"
#include "protocol.h"
#include "http_mock.h"
#include "json.h"
#include "platform.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-55s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("assertion failed: %s", #cond); return; } } while(0)

static api_provider_t *g_api = NULL;

static provider_config_t test_config(bool stream, int max_retries) {
    provider_config_t cfg = {
        .api_key     = strdup("mock-key"),
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = stream,
        .max_retries = max_retries,
    };
    return cfg;
}

/* Push a chat completion with explicit usage token counts. */
static void push_response_with_usage(const char *content,
                                     long long prompt_tokens,
                                     long long completion_tokens) {
    json_value_t *root = json_object();
    json_value_t *choices = json_array();
    json_value_t *choice = json_object();
    json_value_t *message = json_object();
    json_object_set(message, "role", json_string("assistant"));
    json_object_set(message, "content",
        content ? json_string(content) : json_null());
    json_object_set(choice, "index", json_number(0));
    json_object_set(choice, "message", message);
    json_object_set(choice, "finish_reason", json_string("stop"));
    json_array_append(choices, choice);
    json_object_set(root, "choices", choices);

    json_value_t *usage = json_object();
    json_object_set(usage, "prompt_tokens", json_number(prompt_tokens));
    json_object_set(usage, "completion_tokens", json_number(completion_tokens));
    json_object_set(usage, "total_tokens",
                    json_number(prompt_tokens + completion_tokens));
    json_object_set(root, "usage", usage);

    char *body = json_stringify(root);
    http_mock_push(200, body);
    free(body);
    json_free(root);
}

/* Push an SSE stream ending with a usage chunk. */
static void push_stream_with_usage(const char *text,
                                   long long prompt_tokens,
                                   long long completion_tokens) {
    char body[8192];
    snprintf(body, sizeof(body),
        "data: {\"id\":\"x\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"%s\"}}]}\n\n"
        "data: {\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":%lld,"
        "\"completion_tokens\":%lld,\"total_tokens\":%lld}}\n\n"
        "data: [DONE]\n\n",
        text, prompt_tokens, completion_tokens,
        prompt_tokens + completion_tokens);
    http_mock_push(200, body);
}

static void test_retry_transient(void) {
    TEST("non-streaming retries 500 then succeeds");
    http_mock_enable();
    http_mock_push(500, "{\"error\":\"boom\"}");
    push_response_with_usage("hello after retry", 100, 50);

    provider_config_t cfg = test_config(false, 2);
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    message_t *resp = agent_chat(agent, "hi");
    CHECK(resp != NULL);
    CHECK(resp->content && strcmp(resp->content, "hello after retry") == 0);
    CHECK(agent->retry_count == 1);
    CHECK(agent->request_count == 1);
    CHECK(agent->prompt_tokens == 100);
    CHECK(agent->completion_tokens == 50);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_no_retry_on_4xx(void) {
    TEST("non-streaming does not retry 4xx");
    http_mock_push(400, "{\"error\":\"bad request\"}");
    push_response_with_usage("should not happen", 1, 1);

    provider_config_t cfg = test_config(false, 3);
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    message_t *resp = agent_chat(agent, "hi");
    CHECK(resp != NULL);
    CHECK(strstr(resp->content, "Error") != NULL);
    CHECK(agent->retry_count == 0);
    CHECK(agent->request_count == 0);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_retry_exhausted(void) {
    TEST("non-streaming gives up after retries exhausted");
    http_mock_push(500, "{\"error\":\"a\"}");
    http_mock_push(503, "{\"error\":\"b\"}");
    http_mock_push(502, "{\"error\":\"c\"}");

    provider_config_t cfg = test_config(false, 2);
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    message_t *resp = agent_chat(agent, "hi");
    CHECK(resp != NULL);
    CHECK(strstr(resp->content, "Error") != NULL);
    CHECK(agent->retry_count == 2);
    CHECK(agent->request_count == 0);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_stream_retry_usage(void) {
    TEST("streaming retries 500 before output + records usage");
    http_mock_push(500, NULL);
    push_stream_with_usage("streamed hello", 200, 80);

    provider_config_t cfg = test_config(true, 2);
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    message_t *resp = agent_chat_stream(agent, "hi", NULL, NULL);
    CHECK(resp != NULL);
    CHECK(resp->content && strcmp(resp->content, "streamed hello") == 0);
    CHECK(agent->retry_count == 1);
    CHECK(agent->request_count == 1);
    CHECK(agent->prompt_tokens == 200);
    CHECK(agent->completion_tokens == 80);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_usage_accumulates(void) {
    TEST("usage accumulates across requests");
    push_response_with_usage("one", 10, 5);
    push_response_with_usage("two", 20, 8);

    provider_config_t cfg = test_config(false, 1);
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    message_t *r1 = agent_chat(agent, "q1");
    message_t *r2 = agent_chat(agent, "q2");
    CHECK(r1 && r2);
    CHECK(agent->request_count == 2);
    CHECK(agent->prompt_tokens == 30);
    CHECK(agent->completion_tokens == 13);

    message_free(r1);
    message_free(r2);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_estimate_tokens(void) {
    TEST("token estimator (ASCII/CJK)");
    CHECK(agent_estimate_tokens(NULL) == 0);
    CHECK(agent_estimate_tokens("") == 0);
    CHECK(agent_estimate_tokens("abcd") == 1);
    char ascii[501];
    memset(ascii, 'a', 500);
    ascii[500] = '\0';
    CHECK(agent_estimate_tokens(ascii) == 125);
    /* 10 CJK chars ≈ 10 tokens */
    CHECK(agent_estimate_tokens("中文测试十个汉字内容") == 10);
    OK();
}

/* Push a big conversation so the estimated size exceeds the limit. */
static void preload_large_conversation(agent_t *agent, int messages) {
    char buf[1200];
    memset(buf, 'x', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (int i = 0; i < messages; i++) {
        message_t m = {
            .role = MSG_ROLE_USER,
            .content = strdup(buf),
        };
        agent_add_message(agent, &m);
        free(m.content);
    }
}

static void test_auto_compact(void) {
    TEST("auto-compact triggers and replaces history");
    http_mock_enable();
    /* 1st request: compaction summary; 2nd: actual chat response */
    http_mock_push_chat_response("COMPRESSED SUMMARY", NULL, NULL, NULL);
    push_response_with_usage("final answer", 30, 10);

    provider_config_t cfg = test_config(false, 1);
    cfg.context_length = 2000;
    cfg.auto_compact = true;
    cfg.compact_ratio = 0.5;   /* limit = 1000 tokens */
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);
    preload_large_conversation(agent, 5);  /* ~5 * 300 tokens = 1500 > 1000 */

    message_t *resp = agent_chat(agent, "continue");
    CHECK(resp != NULL);
    CHECK(resp->content && strcmp(resp->content, "final answer") == 0);
    CHECK(agent->request_count == 2);      /* compaction + chat */
    CHECK(agent->n_messages >= 2 && agent->n_messages <= 4);
    bool found_summary = false;
    for (int i = 0; i < agent->n_messages; i++) {
        if (agent->messages[i].content &&
            strstr(agent->messages[i].content, "Compressed conversation summary"))
            found_summary = true;
    }
    CHECK(found_summary);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

static void test_auto_compact_trim_fallback(void) {
    TEST("auto-compact falls back to trimming on failure");
    http_mock_enable();
    /* Compaction request fails (500, no retries); chat then succeeds */
    http_mock_push(500, "{\"error\":\"compaction boom\"}");
    push_response_with_usage("still works", 5, 5);

    provider_config_t cfg = test_config(false, 0);
    cfg.context_length = 2000;
    cfg.auto_compact = true;
    cfg.compact_ratio = 0.5;
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);
    preload_large_conversation(agent, 8);
    int before = agent->n_messages;

    message_t *resp = agent_chat(agent, "continue");
    CHECK(resp != NULL);
    CHECK(resp->content && strcmp(resp->content, "still works") == 0);
    CHECK(agent->n_messages < before);     /* trimmed */
    CHECK(agent->n_messages >= 2);         /* kept recent + new turns */

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    OK();
}

/* Slow tool used to observe parallel vs sequential execution. */
static char *test_slow_tool(const char *name, const char *args, char **error) {
    (void)name; (void)args; (void)error;
    struct timespec ts = { 0, 400000000 };  /* 400ms */
    nanosleep(&ts, NULL);
    return strdup("{\"done\":true}");
}

/* Push a response with two test_slow tool calls. */
static void push_two_tool_calls(void) {
    json_value_t *root = json_object();
    json_value_t *choices = json_array();
    json_value_t *choice = json_object();
    json_value_t *message = json_object();
    json_object_set(message, "role", json_string("assistant"));
    json_value_t *tcs = json_array();
    for (int i = 0; i < 2; i++) {
        json_value_t *tc = json_object();
        char id[32];
        snprintf(id, sizeof(id), "call_%d", i);
        json_object_set(tc, "id", json_string(id));
        json_object_set(tc, "type", json_string("function"));
        json_value_t *func = json_object();
        json_object_set(func, "name", json_string("test_slow"));
        json_object_set(func, "arguments", json_string("{}"));
        json_object_set(tc, "function", func);
        json_array_append(tcs, tc);
    }
    json_object_set(message, "tool_calls", tcs);
    json_object_set(message, "content", json_null());
    json_object_set(choice, "index", json_number(0));
    json_object_set(choice, "message", message);
    json_object_set(choice, "finish_reason", json_string("tool_calls"));
    json_array_append(choices, choice);
    json_object_set(root, "choices", choices);
    char *body = json_stringify(root);
    http_mock_push(200, body);
    free(body);
    json_free(root);
}

static void test_parallel_tools(void) {
    TEST("parallel tool execution is faster than sequential");
    tool_registry_clear();
    tool_t *slow = tool_create("test_slow", "slow test tool",
                               "{\"type\":\"object\"}", test_slow_tool);
    slow->thread_safe = true;
    tool_registry_add(slow);

    http_mock_enable();
    push_two_tool_calls();
    http_mock_push_chat_response("done", NULL, NULL, NULL);

    provider_config_t cfg = test_config(false, 0);
    cfg.parallel_tools = true;
    agent_t *agent = agent_create(&cfg, g_api);
    free(cfg.api_key); free(cfg.base_url); free(cfg.model);

    int64_t t0 = os_time_ms();
    message_t *resp = agent_chat(agent, "go");
    int64_t elapsed = os_time_ms() - t0;
    CHECK(resp != NULL);
    CHECK(resp->content && strcmp(resp->content, "done") == 0);
    CHECK(elapsed < 700);   /* 2 x 400ms sleeps would take ~800ms sequentially */

    int tool_results = 0;
    for (int i = 0; i < agent->n_messages; i++) {
        if (agent->messages[i].role == MSG_ROLE_TOOL) tool_results++;
    }
    CHECK(tool_results == 2);

    message_free(resp);
    agent_free(agent);
    http_mock_clear();
    tool_registry_clear();
    OK();
}

int main(void) {
    printf("Agent reliability tests:\n");
    provider_init();
    http_init();
    g_api = provider_get_by_name("deepseek");
    if (!g_api) {
        printf("FAIL: deepseek provider not found\n");
        return 1;
    }

    test_retry_transient();
    test_no_retry_on_4xx();
    test_retry_exhausted();
    test_stream_retry_usage();
    test_usage_accumulates();
    test_estimate_tokens();
    test_auto_compact();
    test_auto_compact_trim_fallback();
    test_parallel_tools();

    http_mock_disable();
    http_cleanup();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
