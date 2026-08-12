/*
 * test_subagent.c — Subagent spawning tests
 */
#include "cgent.h"
#include "subagent.h"
#include "http_mock.h"
#include "platform.h"
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-55s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: assertion failed: %s\n", #cond); return; } } while(0)

/* ── Test: subagent basic spawn (live API) ──────────────────────── */

static void test_subagent_simple(void) {
    TEST("subagent simple task (live API)");
    const char *api_key = getenv("CGENT_API_KEY");
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

/* ── Local chat-completion server (for follow-up tests) ─────────── */

static pid_t start_chat_server(int *out_port, const char *a1, const char *a2) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 4) != 0) {
        close(lfd);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) {
        close(lfd);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid == 0) {
        for (int req = 0; req < 2; req++) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd < 0) break;
            char buf[8192];
            size_t total = 0;
            while (total < sizeof(buf) - 1) {
                ssize_t n = read(cfd, buf + total, sizeof(buf) - 1 - total);
                if (n <= 0) break;
                total += n;
                buf[total] = '\0';
                if (strstr(buf, "\r\n\r\n")) break;
            }
            const char *content = req == 0 ? a1 : a2;
            char body[2048];
            snprintf(body, sizeof(body),
                "{\"id\":\"x\",\"object\":\"chat.completion\",\"created\":1,"
                "\"model\":\"m\",\"choices\":[{\"index\":0,"
                "\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},"
                "\"finish_reason\":\"stop\"}]}", content);
            char resp[4096];
            int rlen = snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                strlen(body), body);
            size_t off = 0;
            while (off < (size_t)rlen) {
                ssize_t n = write(cfd, resp + off, (size_t)rlen - off);
                if (n <= 0) break;
                off += n;
            }
            close(cfd);
        }
        close(lfd);
        _exit(0);
    }
    close(lfd);
    return pid;
}

static const char *cgent_binary_path(void) {
    if (access("../cgent", X_OK) == 0) return "../cgent";
    return "/home/p/work/cgent/cgent";
}

static char g_updates[4][512];
static int g_update_count = 0;

static void collect_updates(subagent_event_type_t type, const char *text,
                            void *ctx) {
    (void)ctx;
    if (type == SUBAGENT_EVENT_UPDATE && g_update_count < 4) {
        snprintf(g_updates[g_update_count], sizeof(g_updates[0]), "%s", text);
        g_update_count++;
    }
}

/* ── Test: async handle + follow-up turns ───────────────────────── */

static void test_subagent_followup(void) {
    TEST("subagent follow-up turns (async handle)");
    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    int port = 0;
    pid_t srv = start_chat_server(&port, "first answer", "second answer");
    CHECK(srv > 0);

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    g_update_count = 0;

    subagent_config_t cfg = {
        .provider       = "deepseek",
        .model          = "deepseek-chat",
        .api_key        = "dummy",
        .base_url       = base_url,
        .system_prompt  = "You are a test subagent.",
        .task           = "first task",
        .binary_path    = (char *)cgent_binary_path(),
        .temperature    = 0.0,
        .max_tokens     = 100,
        .timeout_seconds = 30,
        .on_event       = collect_updates,
    };

    char *err = NULL;
    subagent_handle_t *h = subagent_spawn(&cfg, &err);
    CHECK(h != NULL);
    free(err);

    /* Wait for the first-turn result */
    int64_t deadline = os_time_ms() + 20000;
    while (g_update_count < 1 && os_time_ms() < deadline)
        subagent_poll(h, 200);
    CHECK(g_update_count == 1);
    CHECK(strcmp(g_updates[0], "first answer") == 0);

    /* Send a follow-up and wait for the update */
    CHECK(subagent_followup(h, "second task") == 0);
    deadline = os_time_ms() + 20000;
    while (g_update_count < 2 && os_time_ms() < deadline)
        subagent_poll(h, 200);
    CHECK(g_update_count == 2);
    CHECK(strcmp(g_updates[1], "second answer") == 0);

    subagent_result_t *result = subagent_wait(h, 15);
    CHECK(result != NULL);
    CHECK(result->exit_code == 0);
    CHECK(!result->timed_out);
    CHECK(result->output && strcmp(result->output, "first answer") == 0);
    subagent_result_free(result);
    subagent_handle_free(h);

    waitpid(srv, NULL, 0);
    http_cleanup();
    OK();
}

/* ── Test: abort ────────────────────────────────────────────────── */

static void test_subagent_abort(void) {
    TEST("subagent abort kills the child");
    http_init();
    provider_init();
    tool_registry_clear();
    builtin_tools_register();

    int port = 0;
    pid_t srv = start_chat_server(&port, "answer", "answer");
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    subagent_config_t cfg = {
        .provider       = "deepseek",
        .model          = "deepseek-chat",
        .api_key        = "dummy",
        .base_url       = base_url,
        .task           = "long task",
        .binary_path    = (char *)cgent_binary_path(),
        .timeout_seconds = 30,
    };
    char *err = NULL;
    subagent_handle_t *h = subagent_spawn(&cfg, &err);
    CHECK(h != NULL);
    free(err);

    struct timespec ts = { 0, 300000000 }; /* 300ms */
    nanosleep(&ts, NULL);
    subagent_abort(h);
    subagent_handle_free(h);

    kill(srv, SIGKILL);
    waitpid(srv, NULL, 0);
    http_cleanup();
    OK();
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("Subagent tests:\n");

    test_subagent_config();
    test_subagent_simple();
    test_subagent_with_tools();
    test_subagent_followup();
    test_subagent_abort();

    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
