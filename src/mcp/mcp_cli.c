/*
 * mcp_cli.c — `cgent mcp` subcommand: manage MCP servers
 *
 * Usage:
 *   cgent mcp list
 *   cgent mcp add <name> --command <cmd> [--args a,b] [--arg x] [--env K=V] [--cwd dir]
 *   cgent mcp remove <name>
 *   cgent mcp test <name> [--timeout <ms>]
 *   cgent mcp help
 */
#include "mcp.h"
#include "json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Help ───────────────────────────────────────────────────────── */

static void mcp_usage(void) {
    printf("Usage: cgent mcp <command> [options]\n\n");
    printf("Manage MCP (Model Context Protocol) servers.\n\n");
    printf("Commands:\n");
    printf("  list                  List configured MCP servers\n");
    printf("  add <name> ...        Add or update an MCP server\n");
    printf("  remove <name>         Remove an MCP server\n");
    printf("  test <name>           Verify a server (initialize + tools/list)\n");
    printf("  help                  Show this help\n");
    printf("\nadd options:\n");
    printf("  --command <cmd>       Server executable (required)\n");
    printf("  --args <csv>          Comma-separated command arguments\n");
    printf("  --arg <value>         Append a single argument (repeatable)\n");
    printf("  --env <K=V>           Set environment variable (repeatable, or comma-separated)\n");
    printf("  --cwd <dir>           Working directory for the server\n");
    printf("\ntest options:\n");
    printf("  --timeout <ms>        Handshake timeout in milliseconds (default: 15000)\n");
    printf("\nConfig file: ~/.cgent/mcp.json\n");
}

/* ── list ───────────────────────────────────────────────────────── */

static int mcp_cli_list(void) {
    mcp_config_t *cfg = mcp_config_load();
    if (!cfg) {
        fprintf(stderr, "Error: Failed to load MCP config\n");
        return 1;
    }
    if (cfg->count == 0) {
        printf("No MCP servers configured.\n");
        printf("Add one with: cgent mcp add <name> --command <cmd> [--args ...]\n");
        mcp_config_free(cfg);
        return 0;
    }

    printf("%-20s %-30s %-40s %s\n", "NAME", "COMMAND", "ARGS", "ENV/CWD");
    for (int i = 0; i < cfg->count; i++) {
        mcp_server_t *s = &cfg->servers[i];
        char args_buf[1024] = "";
        size_t off = 0;
        for (int a = 0; a < s->arg_count && off < sizeof(args_buf) - 2; a++) {
            off += snprintf(args_buf + off, sizeof(args_buf) - off,
                            "%s%s", a > 0 ? " " : "", s->args[a]);
        }
        char extra[1024] = "";
        size_t eo = 0;
        for (int e = 0; e < s->env_count && eo < sizeof(extra) - 2; e++) {
            eo += snprintf(extra + eo, sizeof(extra) - eo,
                           "%s%s=%s", e > 0 ? " " : "",
                           s->env_keys[e], s->env_values[e]);
        }
        if (s->cwd && s->cwd[0]) {
            eo += snprintf(extra + eo, sizeof(extra) - eo,
                           "%scwd=%s", eo > 0 ? " " : "", s->cwd);
        }
        printf("%-20s %-30s %-40s %s\n",
               s->name, s->command ? s->command : "",
               args_buf, extra);
    }
    printf("\n%d server(s) configured\n", cfg->count);
    mcp_config_free(cfg);
    return 0;
}

/* ── add ────────────────────────────────────────────────────────── */

static int mcp_cli_add(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent mcp add <name> --command <cmd> [options]\n");
        return 1;
    }
    const char *name = argv[0];
    const char *command = NULL;
    const char *cwd = NULL;
    char **args = NULL;
    int arg_count = 0;
    char **env = NULL;
    int env_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--command") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "--args") == 0 && i + 1 < argc) {
            /* Comma-separated argument list */
            char *copy = strdup(argv[++i]);
            char *saveptr;
            for (char *tok = strtok_r(copy, ",", &saveptr); tok;
                 tok = strtok_r(NULL, ",", &saveptr)) {
                args = realloc(args, (arg_count + 1) * sizeof(char *));
                if (!args) break;
                args[arg_count++] = strdup(tok);
            }
            free(copy);
        } else if (strcmp(argv[i], "--arg") == 0 && i + 1 < argc) {
            args = realloc(args, (arg_count + 1) * sizeof(char *));
            if (args) args[arg_count++] = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) {
            char *copy = strdup(argv[++i]);
            char *saveptr;
            for (char *tok = strtok_r(copy, ",", &saveptr); tok;
                 tok = strtok_r(NULL, ",", &saveptr)) {
                env = realloc(env, (env_count + 1) * sizeof(char *));
                if (!env) break;
                env[env_count++] = strdup(tok);
            }
            free(copy);
        } else if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
            cwd = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            for (int j = 0; j < arg_count; j++) free(args[j]);
            free(args);
            for (int j = 0; j < env_count; j++) free(env[j]);
            free(env);
            return 1;
        }
    }

    if (!command) {
        fprintf(stderr, "Error: --command is required\n");
        for (int j = 0; j < arg_count; j++) free(args[j]);
        free(args);
        for (int j = 0; j < env_count; j++) free(env[j]);
        free(env);
        return 1;
    }

    mcp_config_t *cfg = mcp_config_load();
    if (!cfg) {
        fprintf(stderr, "Error: Failed to load MCP config\n");
        return 1;
    }
    char *err = NULL;
    int rc = mcp_config_add(cfg, name, command, args, arg_count,
                            env, env_count, cwd, &err);
    if (rc != 0) {
        fprintf(stderr, "Error: %s\n", err ? err : "failed to add server");
        free(err);
        mcp_config_free(cfg);
        for (int j = 0; j < arg_count; j++) free(args[j]);
        free(args);
        for (int j = 0; j < env_count; j++) free(env[j]);
        free(env);
        return 1;
    }
    if (mcp_config_save(cfg) != 0) {
        fprintf(stderr, "Error: Failed to write ~/.cgent/mcp.json\n");
        mcp_config_free(cfg);
        for (int j = 0; j < arg_count; j++) free(args[j]);
        free(args);
        for (int j = 0; j < env_count; j++) free(env[j]);
        free(env);
        return 1;
    }

    printf("Saved MCP server '%s' (%s", name, command);
    for (int i = 0; i < arg_count; i++) printf(" %s", args[i]);
    printf(") to ~/.cgent/mcp.json\n");
    if (env_count > 0) {
        printf("  env: ");
        for (int i = 0; i < env_count; i++)
            printf("%s%s", i > 0 ? " " : "", env[i]);
        printf("\n");
    }
    if (cwd) printf("  cwd: %s\n", cwd);

    mcp_config_free(cfg);
    for (int j = 0; j < arg_count; j++) free(args[j]);
    free(args);
    for (int j = 0; j < env_count; j++) free(env[j]);
    free(env);
    return 0;
}

/* ── remove ─────────────────────────────────────────────────────── */

static int mcp_cli_remove(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent mcp remove <name>\n");
        return 1;
    }
    const char *name = argv[0];
    mcp_config_t *cfg = mcp_config_load();
    if (!cfg) {
        fprintf(stderr, "Error: Failed to load MCP config\n");
        return 1;
    }
    if (mcp_config_remove(cfg, name) == 0) {
        fprintf(stderr, "MCP server '%s' not found\n", name);
        mcp_config_free(cfg);
        return 1;
    }
    mcp_config_save(cfg);
    printf("Removed MCP server '%s'\n", name);
    mcp_config_free(cfg);
    return 0;
}

/* ── test ───────────────────────────────────────────────────────── */

static int mcp_cli_test(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: cgent mcp test <name> [--timeout <ms>]\n");
        return 1;
    }
    const char *name = argv[0];
    int timeout_ms = 15000;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--timeout") == 0)
            timeout_ms = atoi(argv[i + 1]);
    }
    if (timeout_ms < 1000) timeout_ms = 1000;

    mcp_config_t *cfg = mcp_config_load();
    if (!cfg) {
        fprintf(stderr, "Error: Failed to load MCP config\n");
        return 1;
    }
    mcp_server_t *srv = mcp_config_find(cfg, name);
    if (!srv) {
        fprintf(stderr, "MCP server '%s' not found\n", name);
        mcp_config_free(cfg);
        return 1;
    }

    printf("Testing MCP server '%s' (%s", name,
           srv->command ? srv->command : "");
    for (int i = 0; i < srv->arg_count; i++) printf(" %s", srv->args[i]);
    printf(")...\n");
    fflush(stdout);

    char *result = mcp_server_test(srv, timeout_ms);
    if (!result) {
        fprintf(stderr, "Error: test failed (no result)\n");
        mcp_config_free(cfg);
        return 1;
    }

    json_value_t *root = json_parse(result);
    free(result);
    if (!root) {
        fprintf(stderr, "Error: invalid test result\n");
        mcp_config_free(cfg);
        return 1;
    }

    json_value_t *ok = json_object_get(root, "success");
    if (ok && json_is_bool(ok) && json_bool_value(ok)) {
        json_value_t *ver = json_object_get(root, "version");
        json_value_t *pv = json_object_get(root, "protocol_version");
        json_value_t *tc = json_object_get(root, "tools_count");
        printf("OK: server version=%s protocol=%s tools=%d\n",
               ver && json_is_string(ver) ? json_string_value(ver) : "?",
               pv && json_is_string(pv) ? json_string_value(pv) : "?",
               tc ? (int)json_number_value(tc) : 0);
        json_value_t *tools = json_object_get(root, "tools");
        if (tools && json_is_array(tools)) {
            int n = json_array_length(tools);
            for (int i = 0; i < n; i++) {
                json_value_t *t = json_array_get(tools, i);
                printf("  - %s\n", json_string_value(t));
            }
        }
        json_value_t *serr = json_object_get(root, "stderr");
        if (serr && json_is_string(serr) && json_string_value(serr)[0]) {
            printf("  stderr: %s\n", json_string_value(serr));
        }
        json_free(root);
        mcp_config_free(cfg);
        return 0;
    }

    json_value_t *e = json_object_get(root, "error");
    printf("FAILED: %s\n",
           e && json_is_string(e) ? json_string_value(e) : "unknown error");
    json_value_t *serr = json_object_get(root, "stderr");
    if (serr && json_is_string(serr) && json_string_value(serr)[0]) {
        printf("  stderr: %s\n", json_string_value(serr));
    }
    json_free(root);
    mcp_config_free(cfg);
    return 1;
}

/* ── Entry point ────────────────────────────────────────────────── */

int mcp_main(int argc, char **argv) {
    if (argc < 2) {
        mcp_usage();
        return 1;
    }
    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        mcp_usage();
        return 0;
    }
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
        return mcp_cli_list();
    if (strcmp(cmd, "add") == 0 || strcmp(cmd, "set") == 0)
        return mcp_cli_add(rest_argc, rest_argv);
    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0)
        return mcp_cli_remove(rest_argc, rest_argv);
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "status") == 0)
        return mcp_cli_test(rest_argc, rest_argv);

    fprintf(stderr, "Unknown mcp command: %s\n\n", cmd);
    mcp_usage();
    return 1;
}
