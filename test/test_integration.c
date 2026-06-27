/*
 * test_integration.c — End-to-end integration test with live DeepSeek API
 */
#include "cgent.h"
#include "http_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-50s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: assertion failed: %s\n", #cond); return; } } while(0)

static const char *api_key;

static void test_simple_chat(void) {
    TEST("simple chat (non-streaming)");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    http_init();
    provider_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 50,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent, "Answer concisely with just the answer.");

    message_t *resp = agent_chat(agent, "What is 2+2? Say just the number.");
    CHECK(resp != NULL);
    CHECK(resp->content != NULL);
    CHECK(strstr(resp->content, "4") != NULL);

    message_free(resp);
    agent_free(agent);
    http_cleanup();
    OK();
}

static void test_tool_use_read_file(void) {
    TEST("tool use: read_file");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    FILE *fp = fopen("/tmp/cgent_int_test.txt", "w");
    fprintf(fp, "The secret code is 42.");
    fclose(fp);

    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 200,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent,
        "You have access to tools. Use read_file to read files. "
        "After reading, tell the user what you found concisely.");

    for (int i = 0; i < registry_count(); i++)
        agent_add_tool(agent, registry_get(i));

    message_t *resp = agent_chat(agent,
        "Read the file /tmp/cgent_int_test.txt and tell me the secret code.");

    CHECK(resp != NULL);
    if (resp->content) {
        CHECK(strstr(resp->content, "42") != NULL);
    } else {
        FAIL("response content is NULL");
    }

    message_free(resp);
    agent_free(agent);
    http_cleanup();
    unlink("/tmp/cgent_int_test.txt");
    OK();
}

static void test_bash_integration(void) {
    TEST("tool use: bash");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 200,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent, "Use bash to run commands. Report output concisely.");

    for (int i = 0; i < registry_count(); i++)
        agent_add_tool(agent, registry_get(i));

    message_t *resp = agent_chat(agent,
        "Run 'echo hello_from_cgent' and tell me the result.");

    CHECK(resp != NULL);
    if (resp->content) {
        CHECK(strstr(resp->content, "hello_from_cgent") != NULL);
    } else {
        FAIL("response content is NULL");
    }

    message_free(resp);
    agent_free(agent);
    http_cleanup();
    OK();
}

/* ── Multi-turn chat tests ────────────────────────────────────── */

static void test_multi_turn_context(void) {
    TEST("multi-turn context memory");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    http_init();
    provider_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent, "Be concise. Remember what the user tells you.");

    /* Turn 1: tell the agent a fact */
    message_t *r1 = agent_chat(agent, "My favorite color is blue. Reply OK.");
    CHECK(r1 != NULL);
    CHECK(r1->content != NULL);
    message_free(r1);

    /* Turn 2: ask about the fact */
    message_t *r2 = agent_chat(agent, "What is my favorite color? Answer with just the color name.");
    CHECK(r2 != NULL);
    CHECK(r2->content != NULL);
    CHECK(strstr(r2->content, "blue") != NULL || strstr(r2->content, "Blue") != NULL);

    message_free(r2);
    agent_free(agent);
    http_cleanup();
    OK();
}

static void test_multi_turn_with_tools(void) {
    TEST("multi-turn with tool use");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    /* Create two test files */
    FILE *fp = fopen("/tmp/cgent_mt_a.txt", "w");
    fprintf(fp, "Alpha content");
    fclose(fp);
    fp = fopen("/tmp/cgent_mt_b.txt", "w");
    fprintf(fp, "Beta content");
    fclose(fp);

    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 200,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent,
        "Use read_file to read files. Be concise.");

    for (int i = 0; i < registry_count(); i++)
        agent_add_tool(agent, registry_get(i));

    /* Turn 1: read first file */
    message_t *r1 = agent_chat(agent, "Read /tmp/cgent_mt_a.txt and tell me what it says.");
    CHECK(r1 != NULL);
    CHECK(r1->content != NULL);
    CHECK(strstr(r1->content, "Alpha") != NULL);

    /* Turn 2: read second file, reference first */
    message_t *r2 = agent_chat(agent,
        "Now read /tmp/cgent_mt_b.txt. Tell me what BOTH files contained, "
        "referencing the first file from our previous exchange.");
    CHECK(r2 != NULL);
    CHECK(r2->content != NULL);
    CHECK(strstr(r2->content, "Beta") != NULL);

    message_free(r1);
    message_free(r2);
    agent_free(agent);
    http_cleanup();
    unlink("/tmp/cgent_mt_a.txt");
    unlink("/tmp/cgent_mt_b.txt");
    OK();
}

static void test_multi_turn_mock(void) {
    TEST("multi-turn with mock backend");
    /* Uses mock to test multi-turn conversation state management */

    http_mock_enable();
    provider_init();
    http_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = "mock-key",
        .base_url    = strdup("https://mock.local"),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 100,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent, "You are helpful. Remember context.");

    /* Turn 1: simple chat */
    http_mock_push_chat_response("OK, I'll remember that your name is Alice.", NULL, NULL, NULL);
    message_t *r1 = agent_chat(agent, "My name is Alice.");
    CHECK(r1 != NULL);
    CHECK(strstr(r1->content, "Alice") != NULL);
    message_free(r1);
    http_mock_clear();

    /* Verify agent has messages: user msg + assistant msg = 2 */
    CHECK(agent->n_messages == 2);

    /* Turn 2: follow-up */
    http_mock_push_chat_response("Your name is Alice.", NULL, NULL, NULL);
    message_t *r2 = agent_chat(agent, "What is my name?");
    CHECK(r2 != NULL);
    CHECK(strstr(r2->content, "Alice") != NULL);
    message_free(r2);
    http_mock_clear();

    /* Verify agent has messages: 4 total (2 user + 2 assistant) */
    CHECK(agent->n_messages == 4);

    agent_free(agent);
    http_mock_clear();
    http_mock_disable();
    http_cleanup();
    OK();
}

/* ── Code generation: bubble sort ─────────────────────────────── */

/* Extract C code from a markdown response. Handles ```c ... ``` blocks
 * and also raw code without fences. Returns malloc'd string. */
static char *extract_c_code(const char *text) {
    if (!text) return NULL;

    /* Find opening code fence: ```c or ``` */
    const char *start = strstr(text, "```");
    if (start) {
        /* Skip past ``` and optional language tag (e.g. "c") to newline */
        start += 3;
        /* Skip language identifier if present (non-newline chars after ```) */
        while (*start && *start != '\n' && *start != '\r') start++;
        /* Skip the newline */
        if (*start == '\r') start++;
        if (*start == '\n') start++;

        /* Find closing ``` */
        const char *end = strstr(start, "```");
        if (end) {
            /* Trim trailing whitespace before closing fence */
            size_t len = end - start;
            while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r'))
                len--;
            char *code = malloc(len + 1);
            if (!code) return NULL;
            memcpy(code, start, len);
            code[len] = '\0';
            return code;
        }
    }
    /* No fences — return the whole text */
    return strdup(text);
}

static void test_generate_bubble_sort(void) {
    TEST("generate bubble sort C code (live API)");
    if (!api_key) { printf("SKIP (no API key)\n"); tests--; return; }

    http_init();
    provider_init();

    api_provider_t *api = provider_get_by_name("deepseek");
    CHECK(api != NULL);

    provider_config_t cfg = {
        .api_key     = (char *)api_key,
        .base_url    = strdup(api->default_base_url),
        .model       = strdup("deepseek-chat"),
        .temperature = 0.0,
        .max_tokens  = 1024,
        .stream      = false,
    };

    agent_t *agent = agent_create(&cfg, api);
    CHECK(agent != NULL);
    agent_set_system_prompt(agent,
        "You are a C programmer. When asked to write code, output ONLY "
        "the C source code in a ```c code block. No explanations. "
        "The code must be complete, compilable with gcc -std=c11, "
        "and include a main() that demonstrates the function.");

    message_t *resp = agent_chat(agent,
        "Write a C function `void bubble_sort(int arr[], int n)` that sorts "
        "an array using bubble sort. Include a main() that sorts "
        "{5, 2, 9, 1, 5, 6} and prints each element separated by spaces.");

    CHECK(resp != NULL);
    CHECK(resp->content != NULL);

    /* Extract C code from response */
    char *code = extract_c_code(resp->content);
    CHECK(code != NULL);

    /* Write to file */
    FILE *fp = fopen("/tmp/cgent_bubble_sort.c", "w");
    CHECK(fp != NULL);
    fputs(code, fp);
    fclose(fp);

    /* Compile */
    int compile_rc = system(
        "gcc -std=c11 -Wall -Wextra -o /tmp/cgent_bubble_sort "
        "/tmp/cgent_bubble_sort.c 2>/tmp/cgent_bubble_sort.err");
    CHECK(compile_rc == 0);

    /* Run */
    int run_rc = system(
        "/tmp/cgent_bubble_sort > /tmp/cgent_bubble_sort.out 2>&1");
    CHECK(run_rc == 0);

    /* Read output */
    fp = fopen("/tmp/cgent_bubble_sort.out", "r");
    CHECK(fp != NULL);
    char output[256] = {0};
    if (fgets(output, sizeof(output), fp)) { /* read */ }
    fclose(fp);

    /* Verify sorted output: "1 2 5 5 6 9" */
    CHECK(strstr(output, "1") != NULL);
    CHECK(strstr(output, "2") != NULL);
    CHECK(strstr(output, "5") != NULL);
    CHECK(strstr(output, "6") != NULL);
    CHECK(strstr(output, "9") != NULL);

    /* Verify the numbers are in order */
    const char *expected = "1 2 5 5 6 9";
    bool sorted_ok = (strstr(output, expected) != NULL);
    if (!sorted_ok) {
        /* Accept alternative formats */
        char *stripped = strdup(output);
        /* Remove newline */
        size_t slen = strlen(stripped);
        while (slen > 0 && (stripped[slen-1] == '\n' || stripped[slen-1] == '\r'))
            stripped[--slen] = '\0';
        sorted_ok = (strstr(stripped, "1 2 5 5 6 9") != NULL);
        free(stripped);
    }
    CHECK(sorted_ok);

    free(code);
    message_free(resp);
    agent_free(agent);
    http_cleanup();

    /* Cleanup */
    unlink("/tmp/cgent_bubble_sort.c");
    unlink("/tmp/cgent_bubble_sort");
    unlink("/tmp/cgent_bubble_sort.out");
    unlink("/tmp/cgent_bubble_sort.err");
    OK();
}

static void test_bubble_sort_mock(void) {
    TEST("generate bubble sort (mock backend)");
    /* Verify code extraction and compilation pipeline with canned code */

    const char *mock_code =
        "#include <stdio.h>\n"
        "void bubble_sort(int arr[], int n) {\n"
        "    for (int i = 0; i < n-1; i++)\n"
        "        for (int j = 0; j < n-i-1; j++)\n"
        "            if (arr[j] > arr[j+1]) {\n"
        "                int t = arr[j]; arr[j] = arr[j+1]; arr[j+1] = t;\n"
        "            }\n"
        "}\n"
        "int main() {\n"
        "    int arr[] = {5, 2, 9, 1, 5, 6};\n"
        "    int n = 6;\n"
        "    bubble_sort(arr, n);\n"
        "    for (int i = 0; i < n; i++) printf(\"%d \", arr[i]);\n"
        "    printf(\"\\n\");\n"
        "    return 0;\n"
        "}\n";

    /* Wrap in markdown code fence to test extraction */
    char mock_response[2048];
    snprintf(mock_response, sizeof(mock_response),
             "Here is the code:\n```c\n%s```\n", mock_code);

    char *code = extract_c_code(mock_response);
    CHECK(code != NULL);
    CHECK(strstr(code, "void bubble_sort") != NULL);
    CHECK(strstr(code, "int main") != NULL);

    /* Write, compile, run, verify */
    FILE *fp = fopen("/tmp/cgent_bs_mock.c", "w");
    CHECK(fp != NULL);
    fputs(code, fp);
    fclose(fp);

    int rc = system("gcc -std=c11 -o /tmp/cgent_bs_mock /tmp/cgent_bs_mock.c 2>/dev/null");
    CHECK(rc == 0);

    rc = system("/tmp/cgent_bs_mock > /tmp/cgent_bs_mock.out 2>&1");
    CHECK(rc == 0);

    fp = fopen("/tmp/cgent_bs_mock.out", "r");
    CHECK(fp != NULL);
    char output[256] = {0};
    if (fgets(output, sizeof(output), fp)) { /* read */ }
    fclose(fp);

    CHECK(strstr(output, "1 2 5 5 6 9") != NULL);

    free(code);
    unlink("/tmp/cgent_bs_mock.c");
    unlink("/tmp/cgent_bs_mock");
    unlink("/tmp/cgent_bs_mock.out");
    OK();
}

int main(void) {
    api_key = getenv("CGENT_API_KEY");
    if (!api_key) api_key = getenv("DEEPSEEK_API_KEY");

    printf("Integration tests (DeepSeek API):\n");
    if (!api_key) {
        printf("  SKIP: No API key in environment\n");
        return 0;
    }

    test_simple_chat();
    test_tool_use_read_file();
    test_bash_integration();
    test_multi_turn_context();
    test_multi_turn_with_tools();
    test_multi_turn_mock();
    test_generate_bubble_sort();
    test_bubble_sort_mock();

    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
