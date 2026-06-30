/*
 * main.c — cgent entry point
 *
 * Pure C AI agent — CLI interface.
 * Supports: --query for single-shot, interactive REPL by default.
 */
#include "cgent.h"
#include "subagent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <locale.h>

static void print_version(void) {
    printf("cgent v" CGENT_VERSION " [%s/%s] (gcc " __VERSION__ ")\n",
           os_name(), os_arch());
    printf("Pure C AI agent — Anthropic / OpenAI / DeepSeek compatible\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -p, --provider <name>    API provider: deepseek, openai, anthropic\n");
    printf("                           (default: deepseek)\n");
    printf("  -m, --model <name>       Model name (default: deepseek-chat)\n");
    printf("  -k, --api-key <key>      API key (override for current provider)\n");
    printf("  -u, --base-url <url>     Override API base URL\n");
    printf("  -q, --query <text>       Single query mode (non-interactive)\n");
    printf("  -a, --agent <dir>        Agent directory (default: agents/cgent/)\n");
    printf("  -t, --temperature <t>    Temperature 0.0–2.0 (default: 0.7)\n");
    printf("  -M, --max-tokens <n>     Max output tokens (default: 4096)\n");
    printf("  -n, --no-stream          Disable streaming output\n");
    printf("  -c, --config <path>      Config file path\n");
    printf("  -v, --verbose            Verbose/debug output\n");
    printf("  -h, --help               Show this help\n");
    printf("  -V, --version            Show version\n");
    printf("\nAgent directory:\n");
    printf("  The agent directory must contain an AGENTS.md file\n");
    printf("  which provides the system prompt for the agent.\n");
    printf("\nEnvironment:\n");
    printf("  CGENT_API_KEY            API key for all providers\n");
    printf("  CGENT_MODEL              Default model\n");
    printf("  CGENT_PROVIDER           Default provider\n");
    printf("  CGENT_AGENT_DIR          Agent directory path\n");
    printf("\nConfiguration:\n");
    printf("  ~/.cgent/settings.json   Default config file\n");
    printf("  ~/.cgent/                cgent config & temp directory\n");
    printf("\nExamples:\n");
    printf("  %s -q \"What is 2+2?\"\n", prog);
    printf("  %s -a agents/myagent -q \"Hello\"\n", prog);
    printf("  %s  (starts interactive REPL)\n", prog);
}

/* ── Tab completion ──────────────────────────────────────────────── */

static cgent_config_t *g_completion_cfg = NULL;

/* Find common prefix of two strings */
static char *str_common_prefix(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    char *r = malloc(i + 1);
    memcpy(r, a, i);
    r[i] = '\0';
    return r;
}

static char *tab_complete(const char *input) {
    /* Only complete slash commands */
    if (!input || input[0] != '/') return NULL;

    size_t ilen = strlen(input);

    /* Built-in commands */
    static const char *builtins[] = {
        "/quit", "/exit", "/help", "/clear", "/tools", "/skills", "/model", NULL
    };

    /* Find matches: any builtin or skill that starts with our input */
    const char *matches[64];
    int n_matches = 0;

    for (int i = 0; builtins[i]; i++) {
        if (strncmp(builtins[i], input, ilen) == 0) {
            matches[n_matches++] = builtins[i];
        }
    }

    /* Check skill commands */
    if (g_completion_cfg && g_completion_cfg->skills) {
        for (int i = 0; i < g_completion_cfg->skills->count; i++) {
            /* Build "/skillname" from trigger */
            const char *trigger = g_completion_cfg->skills->skills[i].trigger;
            if (trigger && strncmp(trigger, input, ilen) == 0) {
                matches[n_matches++] = trigger;
            }
        }
    }

    if (n_matches == 0) return NULL;

    if (n_matches == 1) {
        /* Single match — return it with trailing space */
        size_t mlen = strlen(matches[0]);
        char *result = malloc(mlen + 2);
        memcpy(result, matches[0], mlen);
        result[mlen] = ' ';
        result[mlen + 1] = '\0';
        return result;
    }

    /* Multiple matches — show them, return common prefix */
    if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
    for (int i = 0; i < n_matches; i++) {
        if (write(STDOUT_FILENO, "  ", 2) < 0) {}
        if (write(STDOUT_FILENO, matches[i], strlen(matches[i])) < 0) {}
        if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
    }

    /* Compute common prefix of all matches */
    char *common = strdup(matches[0]);
    for (int i = 1; i < n_matches; i++) {
        char *new_common = str_common_prefix(common, matches[i]);
        free(common);
        common = new_common;
    }

    /* Re-display prompt + current input on a clean line */
    if (write(STDOUT_FILENO, "> ", 2) < 0) {}
    if (write(STDOUT_FILENO, common, strlen(common)) < 0) {}

    return common;
}

/* ── Streaming token callback ───────────────────────────────────── */

static void on_token(const char *token, void *ctx) {
    (void)ctx;
    printf("%s", token);
    fflush(stdout);
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Initialize locale from environment for proper UTF-8/GBK handling */
    setlocale(LC_ALL, "");

    /* ── Subagent mode ──────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--subagent") == 0) {
            return subagent_main(argc, argv);
        }
    }

    cli_args_t args = cli_parse(argc, argv);

    if (args.help) {
        print_usage(argv[0]);
        return 0;
    }

    if (args.version) {
        print_version();
        return 0;
    }

    /* Load configuration */
    cgent_config_t *cfg = config_load();
    if (!cfg) {
        fprintf(stderr, "Error: Failed to load configuration\n");
        return 1;
    }
    config_apply_cli(cfg, &args);

    /* Set up tab completion */
    g_completion_cfg = cfg;
    utf8_set_completer(tab_complete);

    /* Re-resolve system prompt after CLI may have changed agent_dir */
    if (args.agent_dir) {
        char *prompt = config_resolve_agent_prompt(cfg->agent_dir);
        if (prompt) {
            free(cfg->system_prompt);
            cfg->system_prompt = prompt;
        } else {
            free(cfg->system_prompt);
            cfg->system_prompt = NULL;
        }
    }

    /* Resolve API key (already done by resolve_provider in config_load,
     * but CLI --api-key takes priority) */
    if (!cfg->api_key) {
        fprintf(stderr, "Error: No API key provided.\n");
        fprintf(stderr, "Set CGENT_API_KEY environment variable\n");
        fprintf(stderr, "or configure ~/.cgent/settings.json, or use --api-key.\n");
        config_free(cfg);
        return 1;
    }

    if (cfg->verbose) {
        fprintf(stderr, "[cgent] model=%s provider=%s stream=%d\n",
                cfg->model, cfg->provider, cfg->stream);
        fprintf(stderr, "[cgent] temperature=%.2f max_tokens=%d\n",
                cfg->temperature, cfg->max_tokens);
        fprintf(stderr, "[cgent] agent_dir=%s\n",
                cfg->agent_dir ? cfg->agent_dir : "(none)");
        fprintf(stderr, "[cgent] %d models, %d skills\n",
                cfg->model_count,
                cfg->skills ? cfg->skills->count : 0);
        if (cfg->skills && cfg->skills->count > 0) {
            for (int i = 0; i < cfg->skills->count; i++) {
                fprintf(stderr, "[cgent]   skill: %s — %s\n",
                        cfg->skills->skills[i].name,
                        cfg->skills->skills[i].description
                            ? cfg->skills->skills[i].description : "");
            }
        }
    }

    /* Initialize subsystems */
    if (http_init() != 0) {
        fprintf(stderr, "Error: Failed to initialize HTTP/TLS\n");
        config_free(cfg);
        return 1;
    }
    provider_init();

    /* Find the provider */
    api_provider_t *api = provider_get_by_name(cfg->provider);
    if (!api) {
        fprintf(stderr, "Error: Unknown provider '%s'. "
                        "Use deepseek, openai, or anthropic.\n",
                cfg->provider);
        config_free(cfg);
        http_cleanup();
        return 1;
    }

    /* Build provider config */
    provider_config_t pcfg = {
        .api_key     = cfg->api_key,
        .base_url    = cfg->base_url ? cfg->base_url : strdup(api->default_base_url),
        .model       = cfg->model ? cfg->model : strdup(api->default_model),
        .temperature = cfg->temperature,
        .max_tokens  = cfg->max_tokens,
        .stream      = cfg->stream,
    };

    /* Create agent */
    agent_t *agent = agent_create(&pcfg, api);
    if (!agent) {
        fprintf(stderr, "Error: Failed to create agent\n");
        config_free(cfg);
        http_cleanup();
        return 1;
    }
    agent->verbose = cfg->verbose;

    /* Set system prompt — loaded from AGENTS.md in agent directory */
    if (cfg->system_prompt && cfg->system_prompt[0]) {
        agent_set_system_prompt(agent, cfg->system_prompt);
        if (cfg->verbose) {
            char *agent_path = os_path_join(cfg->agent_dir, "AGENTS.md");
            fprintf(stderr, "[cgent] System prompt loaded from %s (%zu chars)\n",
                    agent_path ? agent_path : "?",
                    strlen(cfg->system_prompt));
            free(agent_path);
        }
    } else if (cfg->verbose) {
        fprintf(stderr, "[cgent] No AGENTS.md found in %s\n",
                cfg->agent_dir ? cfg->agent_dir : "(none)");
    }

    /* Register built-in tools */
    builtin_tools_register();
    for (int i = 0; i < registry_count(); i++) {
        agent_add_tool(agent, registry_get(i));
    }

    if (cfg->verbose) {
        fprintf(stderr, "[cgent] Registered %d built-in tools\n",
                agent->n_tools);
    }

    int rc = 0;

    if (args.query) {
        /* ── Single-shot mode ──────────────────────────────────── */
        if (cfg->stream) {
            agent_chat_stream(agent, args.query, on_token, NULL);
            printf("\n");
        } else {
            message_t *resp = agent_chat(agent, args.query);
            if (resp && resp->content) {
                printf("%s\n", resp->content);
            }
            message_free(resp);
        }
    } else {
        /* ── Interactive REPL mode ─────────────────────────────── */
        print_version();
        printf("Provider: %s | Model: %s\n", cfg->provider, cfg->model);
        printf("Type /help for commands, /quit to exit, Ctrl-D to end.\n\n");

        while (1) {
            char *line = utf8_readline("> ");
            if (!line) {
                printf("\n");
                break; /* Ctrl-D or EOF */
            }

            /* Trim trailing whitespace (Tab completion may add spaces) */
            size_t linelen = strlen(line);
            while (linelen > 0 && (line[linelen-1] == ' ' || line[linelen-1] == '\t'))
                line[--linelen] = '\0';

            /* Skip empty lines */
            if (line[0] == '\0') { free(line); continue; }

            /* Handle slash commands — set handled=true to skip agent */
            bool handled = false;
            if (line[0] == '/') {
                handled = true;

                /* Exact-match commands */
                if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
                    free(line);
                    break;
                } else if (strcmp(line, "/help") == 0) {
                    printf("Commands:\n");
                    printf("  /quit, /exit  — Exit the REPL\n");
                    printf("  /help         — Show this help\n");
                    printf("  /clear        — Clear conversation history\n");
                    printf("  /tools        — List available tools\n");
                    printf("  /model [name] — List models or switch to <name>\n");
                    printf("  /skills       — List loaded skills\n");
                    if (cfg->skills && cfg->skills->count > 0) {
                        printf("\nSkill commands:\n");
                        for (int i = 0; i < cfg->skills->count; i++) {
                            printf("  /%-14s %s\n",
                                   cfg->skills->skills[i].name,
                                   cfg->skills->skills[i].description
                                       ? cfg->skills->skills[i].description : "");
                        }
                    }
                    printf("\nOr just type a message to chat with the agent.\n");
                } else if (strcmp(line, "/clear") == 0) {
                    for (int i = 0; i < agent->n_messages; i++)
                        message_clear(&agent->messages[i]);
                    agent->n_messages = 0;
                    printf("Conversation cleared.\n");
                } else if (strcmp(line, "/tools") == 0) {
                    printf("Available tools (%d):\n", agent->n_tools);
                    for (int i = 0; i < agent->n_tools; i++)
                        printf("  - %s: %s\n", agent->tools[i].name, agent->tools[i].description);
                } else if (strcmp(line, "/skills") == 0) {
                    if (cfg->skills && cfg->skills->count > 0) {
                        printf("Loaded skills (%d):\n", cfg->skills->count);
                        for (int i = 0; i < cfg->skills->count; i++) {
                            skill_t *s = &cfg->skills->skills[i];
                            printf("  %-24s  %s\n", s->name,
                                   s->description ? s->description : "");
                        }
                    } else {
                        printf("No skills loaded.\n");
                    }
                } else if (strncmp(line, "/model", 6) == 0) {
                    /* /model or /model <name> */
                    const char *rest = line + 6;
                    while (*rest == ' ') rest++;

                    if (*rest == '\0') {
                        /* /model without args — list models */
                        printf("Available models (%d):\n", cfg->model_count);
                        for (int i = 0; i < cfg->model_count; i++) {
                            const char *mark = (i == cfg->active_model) ? " *" : "  ";
                            printf("%s%s (%s)\n", mark,
                                   cfg->models[i].name, cfg->models[i].provider);
                        }
                        printf("Use /model <name> to switch.\n");
                    } else {
                        /* /model <name> — switch model */
                        const char *model_name = rest;
                        if (config_switch_model(cfg, model_name) == 0) {
                            free(agent->provider.api_key);
                            agent->provider.api_key = cfg->api_key ? strdup(cfg->api_key) : NULL;
                            free(agent->provider.base_url);
                            agent->provider.base_url = cfg->base_url ? strdup(cfg->base_url) : NULL;
                            free(agent->provider.model);
                            agent->provider.model = strdup(cfg->model);
                            agent->provider.temperature = cfg->temperature;
                            agent->provider.max_tokens = cfg->max_tokens;
                            agent->provider.stream = cfg->stream;
                            printf("Model changed to: %s (provider: %s)\n",
                                   cfg->model, cfg->provider);
                        } else {
                            printf("Unknown model: %s. Available models:\n", model_name);
                            for (int i = 0; i < cfg->model_count; i++)
                                printf("  - %s (%s)\n", cfg->models[i].name, cfg->models[i].provider);
                        }
                    }
                } else if (cfg->skills && cfg->skills->count > 0) {
                    /* Check if this is a skill command (e.g. /code-review) */
                    char *space = strchr(line, ' ');
                    size_t cmd_len = space ? (size_t)(space - line) : strlen(line);
                    char *cmd_name = strndup(line, cmd_len);
                    skill_t *sk = skills_find_by_trigger(cfg->skills, cmd_name);

                    if (sk) {
                        const char *params = space ? space + 1 : "";
                        while (*params == ' ') params++;

                        /* Build prompt combining skill instruction with user params */
                        size_t task_sz = strlen(sk->instruction) + strlen(params) + 512;
                        char *task_buf = malloc(task_sz);
                        if (params[0]) {
                            snprintf(task_buf, task_sz,
                                     "Invoke skill '%s' with input: %s\n\n%s",
                                     sk->name, params, sk->instruction);
                        } else {
                            snprintf(task_buf, task_sz,
                                     "Invoke skill '%s'.\n\n%s",
                                     sk->name, sk->instruction);
                        }
                        printf("Invoking skill: %s\n", sk->name);
                        handled = false;
                        free(line);
                        line = task_buf;
                    } else {
                        printf("Unknown command: %s (try /help)\n", line);
                    }
                    free(cmd_name);
                } else {
                    printf("Unknown command: %s (try /help)\n", line);
                }

                if (handled) { free(line); continue; }
            }

            /* Send to agent */
            if (!handled) {
                if (cfg->stream) {
                    agent_chat_stream(agent, line, on_token, NULL);
                    printf("\n");
                } else {
                    message_t *resp = agent_chat(agent, line);
                    if (resp) {
                        if (resp->content) printf("%s\n", resp->content);
                        if (resp->n_tool_calls > 0)
                            printf("[Used %d tool(s)]\n", resp->n_tool_calls);
                        message_free(resp);
                    }
                }
            }
            free(line);
        }
    }

    /* Cleanup */
    agent_free(agent);
    config_free(cfg);
    http_cleanup();

    return rc;
}
