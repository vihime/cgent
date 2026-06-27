/*
 * test_subagent.c — Subagent spawning tests
 */
#include "cgent.h"
#include "subagent.h"
#include "http_mock.h"
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-55s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: assertion failed: %s\n", #cond); return; } } while(0)

/* ── Test: subagent basic spawn (live API) ──────────────────────── */

static void test_subagent_simple(void) {
    TEST("subagent simple task (live API)");
    const char *api_key = getenv("CGENT_API_KEY");
    if (!api_key) api_key = getenv("DEEPSEEK_API_KEY");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    http_init();
    provider_init();

    subagent_config_t cfg = {
        .provider       = "deepseek",
        .model          = "deepseek-chat",
        .api_key        = (char *)api_key,
        .system_prompt  = "Answer concisely with just the answer, no explanation.",
        .task           = "What is 7 * 8? Reply with just the number.",
        .binary_path    = "/home/p/work/cgent/cgent",
        .temperature    = 0.0,
        .max_tokens     = 50,
        .timeout_seconds = 60,
    };

    subagent_result_t *result = subagent_run(&cfg);
    CHECK(result != NULL);
    CHECK(result->exit_code == 0);
    CHECK(!result->timed_out);
    CHECK(result->output != NULL);
    CHECK(strstr(result->output, "56") != NULL);

    subagent_result_free(result);
    http_cleanup();
    OK();
}

/* ── Test: subagent with tool use (live API) ────────────────────── */

static void test_subagent_with_tools(void) {
    TEST("subagent with tool use (live API)");
    const char *api_key = getenv("CGENT_API_KEY");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    FILE *fp = fopen("/tmp/cgent_subagent_test.txt", "w");
    fprintf(fp, "the answer is forty-two");
    fclose(fp);

    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    subagent_config_t cfg = {
        .provider       = "deepseek",
        .model          = "deepseek-chat",
        .api_key        = (char *)api_key,
        .system_prompt  = "Use the read_file tool to read files. "
                          "After reading, tell the user what you found concisely.",
        .task           = "Read /tmp/cgent_subagent_test.txt and tell me what it says.",
        .binary_path    = "/home/p/work/cgent/cgent",
        .temperature    = 0.0,
        .max_tokens     = 200,
        .timeout_seconds = 120,
    };

    subagent_result_t *result = subagent_run(&cfg);
    CHECK(result != NULL);
    CHECK(result->exit_code == 0);
    CHECK(!result->timed_out);
    CHECK(result->output != NULL);
    CHECK(strstr(result->output, "forty-two") != NULL);

    subagent_result_free(result);
    http_cleanup();
    unlink("/tmp/cgent_subagent_test.txt");
    OK();
}

/* ── Test: subagent config ──────────────────────────────────────── */

static void test_subagent_config(void) {
    TEST("subagent config struct");
    subagent_config_t cfg = {
        .provider       = "deepseek",
        .model          = "deepseek-chat",
        .api_key        = "test-key",
        .system_prompt  = "You are a test agent.",
        .task           = "Say hello.",
        .temperature    = 0.5,
        .max_tokens     = 100,
        .timeout_seconds = 30,
    };
    CHECK(strcmp(cfg.provider, "deepseek") == 0);
    CHECK(strcmp(cfg.model, "deepseek-chat") == 0);
    CHECK(cfg.temperature == 0.5);
    CHECK(cfg.max_tokens == 100);
    CHECK(cfg.timeout_seconds == 30);
    OK();
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("Subagent tests:\n");

    test_subagent_config();
    test_subagent_simple();
    test_subagent_with_tools();

    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
