/*
 * test_mcp.c — MCP server config CRUD and stdio test client
 */
#include "mcp.h"
#include "json.h"
#include "platform.h"
#include "tools.h"

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

static char g_orig_home[4096];

static void test_home_isolate(void) {
    const char *home = getenv("HOME");
    if (home) snprintf(g_orig_home, sizeof(g_orig_home), "%s", home);
    else g_orig_home[0] = '\0';
    setenv("HOME", "/tmp/cgent_mcp_test_home", 1);
    system("rm -rf /tmp/cgent_mcp_test_home");
}

static void test_home_restore(void) {
    if (g_orig_home[0]) setenv("HOME", g_orig_home, 1);
    else unsetenv("HOME");
    system("rm -rf /tmp/cgent_mcp_test_home");
}

static void reset_mcp_config(void) {
    unlink("/tmp/cgent_mcp_test_home/.cgent/mcp.json");
}

static void test_config_add_save_load(void) {
    TEST("mcp config add/save/load roundtrip");
    mcp_config_t *cfg = mcp_config_load();
    CHECK(cfg != NULL);
    CHECK(cfg->count == 0);

    char *args[] = { (char *)"-y", (char *)"@modelcontextprotocol/server-filesystem" };
    char *env[] = { (char *)"FOO=bar", (char *)"BAZ=qux" };
    char *err = NULL;
    CHECK(mcp_config_add(cfg, "fs", "npx", args, 2, env, 2, "/tmp", &err) == 0);
    CHECK(err == NULL);
    CHECK(mcp_config_save(cfg) == 0);
    mcp_config_free(cfg);

    cfg = mcp_config_load();
    CHECK(cfg != NULL);
    CHECK(cfg->count == 1);
    mcp_server_t *s = mcp_config_find(cfg, "fs");
    CHECK(s != NULL);
    CHECK(strcmp(s->command, "npx") == 0);
    CHECK(s->arg_count == 2);
    CHECK(strcmp(s->args[0], "-y") == 0);
    CHECK(strcmp(s->args[1], "@modelcontextprotocol/server-filesystem") == 0);
    CHECK(s->env_count == 2);
    CHECK(strcmp(s->env_keys[0], "FOO") == 0);
    CHECK(strcmp(s->env_values[0], "bar") == 0);
    CHECK(s->cwd && strcmp(s->cwd, "/tmp") == 0);
    mcp_config_free(cfg);
    OK();
}

static void test_config_update_remove(void) {
    TEST("mcp config update + remove");
    reset_mcp_config();
    mcp_config_t *cfg = mcp_config_load();
    CHECK(cfg != NULL);
    char *err = NULL;
    CHECK(mcp_config_add(cfg, "svc", "echo", NULL, 0, NULL, 0, NULL, &err) == 0);
    CHECK(mcp_config_add(cfg, "svc", "printf", NULL, 0, NULL, 0, "/var", &err) == 0);
    CHECK(cfg->count == 1);
    mcp_server_t *s = mcp_config_find(cfg, "svc");
    CHECK(s != NULL);
    CHECK(strcmp(s->command, "printf") == 0);
    CHECK(s->cwd && strcmp(s->cwd, "/var") == 0);

    CHECK(mcp_config_remove(cfg, "svc") == 1);
    CHECK(mcp_config_remove(cfg, "svc") == 0);
    CHECK(cfg->count == 0);
    mcp_config_free(cfg);
    OK();
}

static void write_fake_server(const char *path) {
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL);
    if (!fp) return;
    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "while IFS= read -r line; do\n");
    fprintf(fp, "  case \"$line\" in\n");
    fprintf(fp, "    *'\"method\":\"initialize\"'*)\n");
    fprintf(fp, "      echo '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake-server\",\"version\":\"1.2.3\"}}}'\n");
    fprintf(fp, "      ;;\n");
    fprintf(fp, "    *'\"method\":\"tools/list\"'*)\n");
    fprintf(fp, "      echo '{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"alpha\",\"description\":\"A\"},{\"name\":\"beta\",\"description\":\"B\"}]}}'\n");
    fprintf(fp, "      ;;\n");
    fprintf(fp, "  esac\n");
    fprintf(fp, "done\n");
    fclose(fp);
    chmod(path, 0755);
}

static void test_server_test_ok(void) {
    TEST("mcp test with fake stdio server");
    reset_mcp_config();
    write_fake_server("/tmp/cgent_mcp_fake.sh");

    mcp_config_t *cfg = mcp_config_load();
    CHECK(cfg != NULL);
    char *err = NULL;
    CHECK(mcp_config_add(cfg, "fake", "/tmp/cgent_mcp_fake.sh",
                         NULL, 0, NULL, 0, NULL, &err) == 0);
    mcp_server_t *s = mcp_config_find(cfg, "fake");
    CHECK(s != NULL);

    char *out = mcp_server_test(s, 10000);
    CHECK(out != NULL);
    json_value_t *parsed = json_parse(out);
    CHECK(parsed != NULL);
    json_value_t *ok = parsed ? json_object_get(parsed, "success") : NULL;
    CHECK(ok && json_is_bool(ok) && json_bool_value(ok));
    json_value_t *sv = parsed ? json_object_get(parsed, "server") : NULL;
    CHECK(sv && json_is_string(sv) && strcmp(json_string_value(sv), "fake-server") == 0);
    json_value_t *ver = parsed ? json_object_get(parsed, "version") : NULL;
    CHECK(ver && json_is_string(ver) && strcmp(json_string_value(ver), "1.2.3") == 0);
    json_value_t *pv = parsed ? json_object_get(parsed, "protocol_version") : NULL;
    CHECK(pv && json_is_string(pv) && strcmp(json_string_value(pv), "2024-11-05") == 0);
    json_value_t *tc = parsed ? json_object_get(parsed, "tools_count") : NULL;
    CHECK(tc && json_is_number(tc) && json_number_value(tc) == 2.0);
    json_value_t *tools = parsed ? json_object_get(parsed, "tools") : NULL;
    CHECK(tools && json_is_array(tools) && json_array_length(tools) == 2);
    json_free(parsed);
    free(out);

    mcp_config_free(cfg);
    unlink("/tmp/cgent_mcp_fake.sh");
    OK();
}

static void test_server_test_fail(void) {
    TEST("mcp test with missing executable");
    reset_mcp_config();
    mcp_config_t *cfg = mcp_config_load();
    CHECK(cfg != NULL);
    char *err = NULL;
    CHECK(mcp_config_add(cfg, "bad", "/nonexistent/binary",
                         NULL, 0, NULL, 0, NULL, &err) == 0);
    mcp_server_t *s = mcp_config_find(cfg, "bad");
    CHECK(s != NULL);

    char *out = mcp_server_test(s, 5000);
    CHECK(out != NULL);
    json_value_t *parsed = json_parse(out);
    CHECK(parsed != NULL);
    json_value_t *ok = parsed ? json_object_get(parsed, "success") : NULL;
    CHECK(ok && json_is_bool(ok) && !json_bool_value(ok));
    json_value_t *e = parsed ? json_object_get(parsed, "error") : NULL;
    CHECK(e && json_is_string(e) && json_string_value(e)[0]);
    json_free(parsed);
    free(out);

    mcp_config_free(cfg);
    OK();
}

static void write_fake_server_callable(const char *path) {
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL);
    if (!fp) return;
    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "while IFS= read -r line; do\n");
    fprintf(fp, "  case \"$line\" in\n");
    fprintf(fp, "    *'\"method\":\"initialize\"'*)\n");
    fprintf(fp, "      echo '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake-server\",\"version\":\"1.2.3\"}}}'\n");
    fprintf(fp, "      ;;\n");
    fprintf(fp, "    *'\"method\":\"tools/list\"'*)\n");
    fprintf(fp, "      echo '{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\",\"description\":\"Echo text\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}},{\"name\":\"add\",\"description\":\"Add numbers\",\"inputSchema\":{\"type\":\"object\"}}]}}'\n");
    fprintf(fp, "      ;;\n");
    fprintf(fp, "    *'\"method\":\"tools/call\"'*)\n");
    fprintf(fp, "      echo '{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"echo tool ran\"}]}}'\n");
    fprintf(fp, "      ;;\n");
    fprintf(fp, "  esac\n");
    fprintf(fp, "done\n");
    fclose(fp);
    chmod(path, 0755);
}

static void test_bridge_tools(void) {
    TEST("mcp bridge registers + forwards tools/call");
    reset_mcp_config();
    write_fake_server_callable("/tmp/cgent_mcp_fake2.sh");

    mcp_config_t *cfg = mcp_config_load();
    CHECK(cfg != NULL);
    char *err = NULL;
    CHECK(mcp_config_add(cfg, "fakesrv", "/tmp/cgent_mcp_fake2.sh",
                         NULL, 0, NULL, 0, NULL, &err) == 0);

    tool_registry_clear();
    mcp_bridge_t *bridges = NULL;
    int count = 0;
    const char *names[] = { "fakesrv" };
    mcp_bridges_start(cfg, names, 1, false, &bridges, &count);
    CHECK(count == 1);
    CHECK(tool_registry_find("fakesrv__echo") != NULL);
    CHECK(tool_registry_find("fakesrv__add") != NULL);

    char *result = tool_execute("fakesrv__echo", "{\"text\":\"hello\"}",
                                10000, &err);
    CHECK(result != NULL);
    json_value_t *parsed = json_parse(result);
    CHECK(parsed != NULL);
    json_value_t *content = parsed ? json_object_get(parsed, "content") : NULL;
    CHECK(content && json_is_string(content));
    CHECK(content && strstr(json_string_value(content), "echo tool ran") != NULL);
    json_value_t *is_err = parsed ? json_object_get(parsed, "is_error") : NULL;
    CHECK(is_err && json_is_bool(is_err) && !json_bool_value(is_err));
    json_free(parsed);
    free(result);

    mcp_bridges_stop(bridges, count);
    CHECK(tool_registry_find("fakesrv__echo") == NULL);
    CHECK(tool_registry_find("fakesrv__add") == NULL);
    mcp_config_free(cfg);
    unlink("/tmp/cgent_mcp_fake2.sh");
    OK();
}

int main(void) {
    printf("MCP tests:\n");
    test_home_isolate();
    test_config_add_save_load();
    test_config_update_remove();
    test_server_test_ok();
    test_server_test_fail();
    test_bridge_tools();
    test_home_restore();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
