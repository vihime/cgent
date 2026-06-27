/*
 * test_message.c — Message lifecycle unit tests
 */
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-50s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("assertion failed: %s", #cond); return; } } while(0)

static void test_create_free(void) {
    TEST("create and free");
    message_t *msg = message_create(MSG_ROLE_USER, "hello");
    CHECK(msg != NULL);
    CHECK(msg->role == MSG_ROLE_USER);
    CHECK(strcmp(msg->content, "hello") == 0);
    CHECK(msg->n_tool_calls == 0);
    message_free(msg);
    OK();
}

static void test_copy(void) {
    TEST("deep copy");
    message_t *orig = message_create(MSG_ROLE_ASSISTANT, "test content");
    message_add_tool_call(orig, "call_1", "my_tool", "{\"arg\":1}");
    message_add_tool_call(orig, "call_2", "other_tool", "{}");
    CHECK(orig->n_tool_calls == 2);

    message_t *copy = message_copy(orig);
    CHECK(copy != NULL);
    CHECK(strcmp(copy->content, "test content") == 0);
    CHECK(copy->n_tool_calls == 2);
    CHECK(strcmp(copy->tool_calls[0].id, "call_1") == 0);
    CHECK(strcmp(copy->tool_calls[0].name, "my_tool") == 0);
    CHECK(strcmp(copy->tool_calls[0].arguments, "{\"arg\":1}") == 0);
    CHECK(strcmp(copy->tool_calls[1].id, "call_2") == 0);

    /* Verify deep copy: free original, copy should still be valid */
    message_free(orig);
    CHECK(strcmp(copy->content, "test content") == 0);
    CHECK(strcmp(copy->tool_calls[0].id, "call_1") == 0);

    message_free(copy);
    OK();
}

static void test_tool_result(void) {
    TEST("tool result message");
    message_t *msg = message_create(MSG_ROLE_TOOL, NULL);
    message_add_tool_result(msg, "call_abc", "result text", false);
    CHECK(msg->n_tool_results == 1);
    CHECK(strcmp(msg->tool_results[0].tool_call_id, "call_abc") == 0);
    CHECK(strcmp(msg->tool_results[0].content, "result text") == 0);
    CHECK(msg->tool_results[0].is_error == false);
    message_free(msg);
    OK();
}

static void test_assistant_tool_calls(void) {
    TEST("tool calls on assistant role");
    message_t *msg = message_create(MSG_ROLE_ASSISTANT, NULL);
    message_add_tool_call(msg, "id1", "func1", "{}");
    CHECK(msg->n_tool_calls == 1);
    CHECK(msg->content == NULL);
    message_free(msg);
    OK();
}

static void test_message_clear(void) {
    TEST("message_clear on array element");
    message_t *msg = message_create(MSG_ROLE_USER, "test");
    message_clear(msg);
    CHECK(msg->content == NULL);
    CHECK(msg->n_tool_calls == 0);
    CHECK(msg->n_tool_results == 0);
    /* Should be safe to free the struct after clear */
    free(msg);
    OK();
}

int main(void) {
    printf("Message tests:\n");
    test_create_free();
    test_copy();
    test_tool_result();
    test_assistant_tool_calls();
    test_message_clear();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
