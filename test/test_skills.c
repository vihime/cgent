/*
 * test_skills.c — `cgent skill` CLI and skill directory management
 */
#include "skills.h"
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

static char g_orig_home[4096];

static void test_home_isolate(void) {
    const char *home = getenv("HOME");
    if (home) snprintf(g_orig_home, sizeof(g_orig_home), "%s", home);
    else g_orig_home[0] = '\0';
    setenv("HOME", "/tmp/cgent_skill_test_home", 1);
    system("rm -rf /tmp/cgent_skill_test_home");
}

static void test_home_restore(void) {
    if (g_orig_home[0]) setenv("HOME", g_orig_home, 1);
    else unsetenv("HOME");
    system("rm -rf /tmp/cgent_skill_test_home");
}

static const char *skill_md_path(void) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
             "/tmp/cgent_skill_test_home/.cgent/skills/review/SKILL.md");
    return buf;
}

static void test_skill_add(void) {
    TEST("skill add creates SKILL.md");
    char *argv_add[] = {
        (char *)"skill", (char *)"add", (char *)"review",
        (char *)"--description", (char *)"Review code for issues",
        (char *)"--instruction", (char *)"You review code carefully.",
        NULL
    };
    CHECK(skill_main(7, argv_add) == 0);
    CHECK(os_path_exists(skill_md_path()));

    char *dir = os_path_join("/tmp/cgent_skill_test_home/.cgent", "skills");
    CHECK(dir != NULL);
    skill_list_t *list = skills_load_directory(dir);
    free(dir);
    CHECK(list != NULL);
    CHECK(list->count == 1);
    skill_t *s = skills_find(list, "review");
    CHECK(s != NULL);
    CHECK(strcmp(s->name, "review") == 0);
    CHECK(strcmp(s->description, "Review code for issues") == 0);
    CHECK(strstr(s->instruction, "You review code carefully.") != NULL);
    CHECK(s->trigger && strcmp(s->trigger, "/review") == 0);
    skills_free(list);
    OK();
}

static void test_skill_add_duplicate(void) {
    TEST("skill add duplicate + --force");
    char *argv_dup[] = {
        (char *)"skill", (char *)"add", (char *)"review",
        (char *)"--description", (char *)"dup", NULL
    };
    CHECK(skill_main(5, argv_dup) != 0);   /* fails without --force */

    char *argv_force[] = {
        (char *)"skill", (char *)"add", (char *)"review",
        (char *)"--description", (char *)"updated",
        (char *)"--force", NULL
    };
    CHECK(skill_main(6, argv_force) == 0);
    OK();
}

static void test_skill_invalid_name(void) {
    TEST("skill add rejects invalid names");
    char *argv_bad[] = {
        (char *)"skill", (char *)"add", (char *)"../evil",
        (char *)"--description", (char *)"x", NULL
    };
    CHECK(skill_main(5, argv_bad) != 0);
    CHECK(!os_path_exists("/tmp/cgent_skill_test_home/.cgent/skills/../evil"));
    OK();
}

static void test_skill_show_remove(void) {
    TEST("skill show + remove");
    char *argv_show[] = {
        (char *)"skill", (char *)"show", (char *)"review", NULL
    };
    CHECK(skill_main(3, argv_show) == 0);
    char *argv_missing[] = {
        (char *)"skill", (char *)"show", (char *)"nope", NULL
    };
    CHECK(skill_main(3, argv_missing) != 0);

    char *argv_rm[] = {
        (char *)"skill", (char *)"remove", (char *)"review", NULL
    };
    CHECK(skill_main(3, argv_rm) == 0);
    CHECK(!os_path_exists(skill_md_path()));
    char *argv_rm2[] = {
        (char *)"skill", (char *)"remove", (char *)"review", NULL
    };
    CHECK(skill_main(3, argv_rm2) != 0);
    OK();
}

int main(void) {
    printf("Skill tests:\n");
    test_home_isolate();
    test_skill_add();
    test_skill_add_duplicate();
    test_skill_invalid_name();
    test_skill_show_remove();
    test_home_restore();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
