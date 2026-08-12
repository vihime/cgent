/*
 * test_tools.c — Tool registry and built-in tools unit tests
 */
#include "tools.h"
#include "json.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void test_edit_tool(void) {
    TEST("edit tool");
    /* Create test file */
    FILE *fp = fopen("/tmp/cgent_edit_test.c", "w");
    fprintf(fp, "int main() {\n    return 0;\n}\n");
    fclose(fp);

    char *error = NULL;
    char *result = tool_execute("edit",
        "{\"file_path\":\"/tmp/cgent_edit_test.c\","
        "\"old_string\":\"return 0;\","
        "\"new_string\":\"return 1;\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);

    /* Verify edit */
    fp = fopen("/tmp/cgent_edit_test.c", "r");
    CHECK(fp != NULL);
    char buf[256] = {0};
    fread(buf, 1, sizeof(buf)-1, fp);
    fclose(fp);
    CHECK(strstr(buf, "return 1;") != NULL);
    CHECK(strstr(buf, "return 0;") == NULL);

    free(result);
    unlink("/tmp/cgent_edit_test.c");
    OK();
}

static void test_glob_tool(void) {
    TEST("glob tool");
    char *error = NULL;
    char *result = tool_execute("glob",
        "{\"pattern\":\"*.c\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);
    /* Should find at least main.c */
    CHECK(strstr(result, ".c") != NULL);
    free(result);
    OK();
}

static void test_grep_tool(void) {
    TEST("grep tool");
    char *error = NULL;
    char *result = tool_execute("grep",
        "{\"pattern\":\"main\",\"include\":\"*.c\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(error == NULL);
    /* Should find main somewhere */
    CHECK(strstr(result, "main") != NULL);
    free(result);
    OK();
}

static void test_bash_timeout(void) {
    TEST("exec timeout");
    int ec = 0;
    int64_t t0 = os_time_ms();
    char *out = os_exec_capture_timeout("sleep 5", 500, &ec);
    int64_t dt = os_time_ms() - t0;
    CHECK(out != NULL);
    CHECK(ec == 124);
    CHECK(strstr(out, "timed out") != NULL);
    CHECK(dt < 3000); /* must not wait for the full sleep */
    free(out);
    OK();
}

static void test_shell_quoting(void) {
    TEST("tool args shell-quoted (no injection)");
    unlink("/tmp/cgent_pwned");
    char *error = NULL;
    char *result = tool_execute("grep",
        "{\"pattern\":\"'; touch /tmp/cgent_pwned; '\",\"include\":\"*.c\"}",
        5000, &error);
    CHECK(result != NULL);
    CHECK(access("/tmp/cgent_pwned", F_OK) != 0); /* must not exist */
    free(result);
    if (error) free(error);
    OK();
}

static void test_list_dir_tool(void) {
    TEST("list_dir tool");
    mkdir("/tmp/cgent_ls_test", 0755);
    FILE *fp = fopen("/tmp/cgent_ls_test/a.txt", "w");
    if (fp) { fputs("x", fp); fclose(fp); }
    mkdir("/tmp/cgent_ls_test/sub", 0755);

    char *error = NULL;
    char *result = tool_execute("list_dir",
        "{\"path\":\"/tmp/cgent_ls_test\"}", 5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "a.txt") != NULL);
    CHECK(strstr(result, "sub") != NULL);
    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    CHECK(json_number_value(json_object_get(parsed, "count")) >= 2.0);
    json_free(parsed);
    free(result);

    unlink("/tmp/cgent_ls_test/a.txt");
    rmdir("/tmp/cgent_ls_test/sub");
    rmdir("/tmp/cgent_ls_test");
    OK();
}

static void test_apply_patch_update(void) {
    TEST("apply_patch update");
    FILE *fp = fopen("/tmp/cgent_patch_test.txt", "w");
    if (fp) {
        fprintf(fp, "line one\nline two\nline three\n");
        fclose(fp);
    }

    char *error = NULL;
    char *result = tool_execute("apply_patch",
        "{\"patch\":\"--- /tmp/cgent_patch_test.txt\\n"
        "+++ /tmp/cgent_patch_test.txt\\n"
        "@@ -1,3 +1,3 @@\\n"
        " line one\\n"
        "-line two\\n"
        "+line TWO\\n"
        " line three\\n\"}",
        5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "\"success\":true") != NULL);

    fp = fopen("/tmp/cgent_patch_test.txt", "r");
    CHECK(fp != NULL);
    char buf[256] = {0};
    if (fp) {
        fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
    }
    CHECK(strstr(buf, "line TWO") != NULL);
    CHECK(strstr(buf, "line two") == NULL);
    CHECK(strstr(buf, "line one") != NULL);

    free(result);
    unlink("/tmp/cgent_patch_test.txt");
    OK();
}

static void test_apply_patch_add_delete(void) {
    TEST("apply_patch add + delete");
    char *error = NULL;

    char *result = tool_execute("apply_patch",
        "{\"patch\":\"--- /dev/null\\n"
        "+++ /tmp/cgent_patch_new.txt\\n"
        "@@ -0,0 +1,2 @@\\n"
        "+hello\\n"
        "+world\\n\"}",
        5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "\"success\":true") != NULL);
    CHECK(access("/tmp/cgent_patch_new.txt", F_OK) == 0);
    free(result);

    result = tool_execute("apply_patch",
        "{\"patch\":\"--- /tmp/cgent_patch_new.txt\\n"
        "+++ /dev/null\\n"
        "@@ -1,2 +0,0 @@\\n"
        "-hello\\n"
        "-world\\n\"}",
        5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "\"success\":true") != NULL);
    CHECK(access("/tmp/cgent_patch_new.txt", F_OK) != 0);
    free(result);
    if (error) free(error);
    OK();
}

static void test_apply_patch_mismatch(void) {
    TEST("apply_patch context mismatch fails cleanly");
    FILE *fp = fopen("/tmp/cgent_patch_mismatch.txt", "w");
    if (fp) {
        fprintf(fp, "aaa\nbbb\n");
        fclose(fp);
    }

    char *error = NULL;
    char *result = tool_execute("apply_patch",
        "{\"patch\":\"--- /tmp/cgent_patch_mismatch.txt\\n"
        "+++ /tmp/cgent_patch_mismatch.txt\\n"
        "@@ -1,2 +1,2 @@\\n"
        " xxx\\n"
        "-yyy\\n"
        "+zzz\\n\"}",
        5000, &error);
    CHECK(result != NULL);
    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    json_value_t *succ = parsed ? json_object_get(parsed, "success") : NULL;
    CHECK(succ != NULL && json_is_bool(succ));
    CHECK(succ && json_bool_value(succ) == false);
    json_free(parsed);
    free(result);
    if (error) free(error);
    unlink("/tmp/cgent_patch_mismatch.txt");
    OK();
}

static void test_git_tools(void) {
    TEST("git_status / git_diff / git_log");
    char old_cwd[4096];
    if (!getcwd(old_cwd, sizeof(old_cwd))) strcpy(old_cwd, ".");

    system("rm -rf /tmp/cgent_git_test && mkdir -p /tmp/cgent_git_test");
    if (chdir("/tmp/cgent_git_test") != 0) {
        FAIL("cannot chdir to test repo");
        return;
    }
    system("git init -q && git config user.email test@cgent && git config user.name cgent-test");
    FILE *fp = fopen("f.txt", "w");
    if (fp) { fprintf(fp, "v1\n"); fclose(fp); }
    system("git add f.txt && git commit -qm first");
    fp = fopen("f.txt", "w");
    if (fp) { fprintf(fp, "v2\n"); fclose(fp); }

    char *error = NULL;
    char *result = tool_execute("git_status", "{}", 5000, &error);
    CHECK(result != NULL);
    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    json_value_t *ok = parsed ? json_object_get(parsed, "success") : NULL;
    CHECK(ok && json_is_bool(ok) && json_bool_value(ok));
    CHECK(parsed && json_number_value(json_object_get(parsed, "count")) >= 1.0);
    CHECK(strstr(result, "f.txt") != NULL);
    json_free(parsed);
    free(result);

    result = tool_execute("git_diff", "{}", 5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "v2") != NULL || strstr(result, "-v1") != NULL);
    free(result);

    result = tool_execute("git_log", "{\"n\":5}", 5000, &error);
    CHECK(result != NULL);
    CHECK(strstr(result, "first") != NULL);
    free(result);
    if (error) free(error);

    chdir(old_cwd);
    system("rm -rf /tmp/cgent_git_test");
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
    test_edit_tool();
    test_glob_tool();
    test_grep_tool();
    test_bash_timeout();
    test_shell_quoting();
    test_list_dir_tool();
    test_apply_patch_update();
    test_apply_patch_add_delete();
    test_apply_patch_mismatch();
    test_git_tools();
    test_tool_not_found();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
