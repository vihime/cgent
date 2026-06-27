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
    printf("  -k, --api-key <key>      API key (or set CGENT_API_KEY env var)\n");
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
    printf("  CGENT_API_KEY            API key\n");
    printf("  DEEPSEEK_API_KEY         DeepSeek API key\n");
    printf("  OPENAI_API_KEY           OpenAI API key\n");
    printf("  ANTHROPIC_API_KEY        Anthropic API key\n");
    printf("  CGENT_MODEL              Default model\n");
    printf("  CGENT_PROVIDER           Default provider\n");
    printf("  CGENT_AGENT_DIR          Agent directory path\n");
    printf("\nExamples:\n");
    printf("  %s -q \"What is 2+2?\"\n", prog);
    printf("  %s -a agents/myagent -q \"Hello\"\n", prog);
    printf("  %s  (starts interactive REPL)\n", prog);
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

    /* Resolve API key */
    if (!cfg->api_key) {
        char *env = os_getenv("CGENT_API_KEY");
        if (!env) env = os_getenv("DEEPSEEK_API_KEY");
        if (!env) env = os_getenv("OPENAI_API_KEY");
        if (!env) env = os_getenv("ANTHROPIC_API_KEY");
        cfg->api_key = env;
    }

    if (!cfg->api_key) {
        fprintf(stderr, "Error: No API key provided.\n");
        fprintf(stderr, "Set CGENT_API_KEY or provider-specific env var, "
                        "or use --api-key.\n");
        config_free(cfg);
        return 1;
    }

    if (cfg->verbose) {
        fprintf(stderr, "[cgent] provider=%s model=%s stream=%d\n",
                cfg->provider, cfg->model, cfg->stream);
        fprintf(stderr, "[cgent] temperature=%.2f max_tokens=%d\n",
                cfg->temperature, cfg->max_tokens);
        fprintf(stderr, "[cgent] agent_dir=%s\n",
                cfg->agent_dir ? cfg->agent_dir : "(none)");
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

            /* Skip empty lines */
            if (line[0] == '\0') { free(line); continue; }

            /* Handle slash commands — set handled=true to skip agent */
            bool handled = false;
            if (line[0] == '/') {
                handled = true;
                if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
                    free(line);
                    break;
                } else if (strcmp(line, "/help") == 0) {
                    printf("Commands:\n");
                    printf("  /quit, /exit  — Exit the REPL\n");
                    printf("  /help         — Show this help\n");
                    printf("  /clear        — Clear conversation history\n");
                    printf("  /tools        — List available tools\n");
                    printf("  /model <name> — Switch model\n");
                    printf("\nOr just type a message to chat with the agent.\n");
                } else if (strcmp(line, "/clear") == 0) {
                    for (int i = 0; i < agent->n_messages; i++)
                        message_clear(&agent->messages[i]);
                    agent->n_messages = 0;
                    printf("Conversation cleared.\n");
                } else if (strcmp(line, "/tools") == 0) {
                    printf("Available tools (%d):\n", agent->n_tools);
                    for (int i = 0; i < agent->n_tools; i++) {
                        printf("  - %s: %s\n",
                               agent->tools[i].name,
                               agent->tools[i].description);
                    }
                } else if (strncmp(line, "/model ", 7) == 0) {
                    free(agent->provider.model);
                    agent->provider.model = strdup(line + 7);
                    printf("Model changed to: %s\n", agent->provider.model);
                } else {
                    printf("Unknown command: %s (try /help)\n", line);
                }
            }

            /* Send to agent (unless handled as slash command) */
            if (!handled) {
                if (cfg->stream) {
                    agent_chat_stream(agent, line, on_token, NULL);
                    printf("\n");
                } else {
                    message_t *resp = agent_chat(agent, line);
                    if (resp) {
                        if (resp->content)
                            printf("%s\n", resp->content);
                        if (resp->n_tool_calls > 0) {
                            printf("[Used %d tool(s)]\n", resp->n_tool_calls);
                        }
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
