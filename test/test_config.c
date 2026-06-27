/*
 * test_config.c — Config and AGENTS.md parser unit tests
 */
#include "config.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-50s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("assertion failed: %s", #cond); return; } } while(0)

static void test_agent_md_parse_basic(void) {
    TEST("AGENTS.md parse basic");
    const char *path = "/tmp/test_agent_md_basic.md";
    FILE *fp = fopen(path, "w");
    fprintf(fp, "---\n");
    fprintf(fp, "name: test-agent\n");
    fprintf(fp, "description: A test agent\n");
    fprintf(fp, "model: deepseek-chat\n");
    fprintf(fp, "---\n");
    fprintf(fp, "You are a helpful assistant.\n");
    fprintf(fp, "Follow these rules.\n");
    fclose(fp);

    agent_md_t *am = agent_md_parse(path);
    CHECK(am != NULL);
    CHECK(strcmp(am->name, "test-agent") == 0);
    CHECK(strcmp(am->description, "A test agent") == 0);
    CHECK(strcmp(am->model, "deepseek-chat") == 0);
    CHECK(am->instruction != NULL);
    CHECK(strstr(am->instruction, "helpful assistant") != NULL);

    agent_md_free(am);
    unlink(path);
    OK();
}

static void test_agent_md_with_lists(void) {
    TEST("AGENTS.md parse with lists");
    const char *path = "/tmp/test_agent_md_lists.md";
    FILE *fp = fopen(path, "w");
    fprintf(fp, "---\n");
    fprintf(fp, "name: multi-agent\n");
    fprintf(fp, "skills:\n");
    fprintf(fp, "  - code-review\n");
    fprintf(fp, "  - simplify\n");
    fprintf(fp, "mcp_servers:\n");
    fprintf(fp, "  - filesystem\n");
    fprintf(fp, "  - fetch\n");
    fprintf(fp, "---\n");
    fprintf(fp, "Do stuff.\n");
    fclose(fp);

    agent_md_t *am = agent_md_parse(path);
    CHECK(am != NULL);
    CHECK(am->skills_count == 2);
    CHECK(am->skills[0] != NULL);
    CHECK(strcmp(am->skills[0], "code-review") == 0);
    CHECK(strcmp(am->skills[1], "simplify") == 0);
    CHECK(am->mcp_servers_count == 2);
    CHECK(strcmp(am->mcp_servers[0], "filesystem") == 0);

    agent_md_free(am);
    unlink(path);
    OK();
}

static void test_agent_md_no_frontmatter(void) {
    TEST("AGENTS.md no frontmatter");
    const char *path = "/tmp/test_agent_md_nofm.md";
    FILE *fp = fopen(path, "w");
    fprintf(fp, "Just instructions, no frontmatter.\n");
    fclose(fp);

    agent_md_t *am = agent_md_parse(path);
    CHECK(am != NULL);
    CHECK(am->name == NULL);
    CHECK(am->instruction != NULL);
    CHECK(strstr(am->instruction, "Just instructions") != NULL);

    agent_md_free(am);
    unlink(path);
    OK();
}

static void test_config_apply_cli(void) {
    TEST("config_apply_cli merging");
    cgent_config_t *cfg = calloc(1, sizeof(cgent_config_t));
    cfg->provider = strdup("deepseek");
    cfg->temperature = 0.7;
    cfg->stream = true;

    cli_args_t args = {
        .provider     = "openai",
        .model        = "gpt-4o",
        .agent_dir    = "agents/testbot/",
        .temperature  = 0.3,
        .stream       = false,
        .max_tokens   = 2048,
    };

    config_apply_cli(cfg, &args);
    CHECK(strcmp(cfg->provider, "openai") == 0);
    CHECK(strcmp(cfg->model, "gpt-4o") == 0);
    CHECK(strcmp(cfg->agent_dir, "agents/testbot/") == 0);
    CHECK(cfg->temperature == 0.3);
    CHECK(cfg->stream == false);
    CHECK(cfg->max_tokens == 2048);

    config_free(cfg);
    OK();
}

static void test_os_functions(void) {
    TEST("OS helper functions");
    CHECK(strlen(os_name()) > 0);
    CHECK(strlen(os_arch()) > 0);

    char *home = os_home_dir();
    CHECK(home != NULL);
    CHECK(strlen(home) > 0);
    free(home);

    char *cfg = os_config_dir();
    CHECK(cfg != NULL);
    free(cfg);

    char *joined = os_path_join("/foo", "bar");
    CHECK(strcmp(joined, "/foo/bar") == 0);
    free(joined);

    CHECK(os_path_exists("/") == true);
    CHECK(os_path_exists("/nonexistent_xyz123") == false);
    OK();
}

int main(void) {
    printf("Config tests:\n");
    test_agent_md_parse_basic();
    test_agent_md_with_lists();
    test_agent_md_no_frontmatter();
    test_config_apply_cli();
    test_os_functions();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
