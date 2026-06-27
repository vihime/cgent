/*
 * test_tools.c — Tool registry and built-in tools unit tests
 */
#include "tools.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-50s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("assertion failed: %s", #cond); return; } } while(0)

static void test_registry_add_find(void) {
    TEST("registry add and find");
    tool_registry_clear();

    tool_t *t = tool_create("test_tool", "A test tool",
        "{\"type\":\"object\"}", NULL);
    CHECK(tool_registry_add(t) == 0);
    CHECK(registry_count() >= 1);
    CHECK(tool_registry_find("test_tool") == t);
    CHECK(tool_registry_find("nonexistent") == NULL);
    OK();
}

static void test_read_file(void) {
    TEST("read_file tool");
    builtin_tools_register();

    FILE *fp = fopen("/tmp/cgent_test_read.txt", "w");
    fprintf(fp, "Hello, cgent test!");
    fclose(fp);

    char *error = NULL;
    char *result = tool_execute("read_file",
        "{\"path\":\"/tmp/cgent_test_read.txt\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "Hello, cgent test!") != NULL);
    CHECK(error == NULL);

    free(result);
    unlink("/tmp/cgent_test_read.txt");
    OK();
}

static void test_write_file(void) {
    TEST("write_file tool");
    char *error = NULL;
    char *result = tool_execute("write_file",
        "{\"path\":\"/tmp/cgent_test_write.txt\",\"content\":\"test output\"}",
        5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);

    FILE *fp = fopen("/tmp/cgent_test_write.txt", "r");
    CHECK(fp != NULL);
    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), fp)) { /* read */ }
    fclose(fp);
    CHECK(strcmp(buf, "test output") == 0);

    free(result);
    unlink("/tmp/cgent_test_write.txt");
    OK();
}

static void test_bash_tool(void) {
    TEST("bash tool");
    char *error = NULL;
    char *result = tool_execute("bash",
        "{\"command\":\"echo hello bash\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);
    CHECK(strstr(result, "hello bash") != NULL);

    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    CHECK(json_number_value(json_object_get(parsed, "exit_code")) == 0.0);

    json_free(parsed);
    free(result);
    OK();
}

static void test_think_tool(void) {
    TEST("think tool");
    char *error = NULL;
    char *result = tool_execute("think",
        "{\"thought\":\"I should check if this works\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);

    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    CHECK(json_bool_value(json_object_get(parsed, "acknowledged")) == true);

    json_free(parsed);
    free(result);
    OK();
}

static void test_tool_not_found(void) {
    TEST("tool not found");
    char *error = NULL;
    char *result = tool_execute("nonexistent_tool", "{}", 5000, &error);
    CHECK(result == NULL);
    CHECK(error != NULL);
    CHECK(strcmp(error, "Tool not found") == 0);
    free(error);
    OK();
}

int main(void) {
    printf("Tool tests:\n");
    test_registry_add_find();
    test_read_file();
    test_write_file();
    test_bash_tool();
    test_think_tool();
    test_tool_not_found();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
