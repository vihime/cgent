/*
 * main.c — cgent entry point
 *
 * Pure C AI agent — CLI interface.
 * Supports: --query for single-shot, interactive REPL by default.
 */
#include "cgent.h"
#include "subagent.h"
#include "session.h"
#include "mcp.h"
#include "skills.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <locale.h>
#include <dirent.h>

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
    printf("  -m, --model <name>       Model name (default: deepseek-v4-flash)\n");
    printf("  -k, --api-key <key>      API key (override for current provider)\n");
    printf("  -u, --base-url <url>     Override API base URL\n");
    printf("  -q, --query <text>       Single query mode (non-interactive)\n");
    printf("  -a, --agent <dir>        Agent directory (default: agents/cgent/)\n");
    printf("  -t, --temperature <t>    Temperature 0.0–2.0 (default: 0.7)\n");
    printf("  -M, --max-tokens <n>     Max output tokens (default: 4096)\n");
    printf("      --retries <n>         Transient-failure retries per request (default: 3)\n");
    printf("      --no-auto-compact     Disable automatic context compaction\n");
    printf("      --no-parallel-tools   Execute tool calls sequentially\n");
    printf("  -n, --no-stream          Disable streaming output\n");
    printf("  -y, --yes                Skip approval prompts for risky tools\n");
    printf("  -c, --config <path>      Config file path\n");
    printf("  -r, --resume <uuid>      Resume session by UUID\n");
    printf("  -v, --verbose            Verbose/debug output\n");
    printf("  -h, --help               Show this help\n");
    printf("  -V, --version            Show version\n");
    printf("      --mcp <name>         Enable MCP server by config name (repeatable)\n");
    printf("      --mcp-all            Enable all configured MCP servers\n");
    printf("\nSubcommands:\n");
    printf("  config <provider> <key>  Configure API key for a provider\n");
    printf("  mcp <command>            Manage MCP servers (list/add/remove/test)\n");
    printf("  skill <command>          Manage skills (list/add/remove/show)\n");
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
        "/quit", "/exit", "/help", "/clear", "/tools", "/agents", "/skills",
        "/model", "/context", "/usage", "/compact", NULL
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

/* ── Usage accounting ───────────────────────────────────────────── */

/* Fold the agent's per-turn counters into the session totals. */
static void session_track_usage(session_t *session, const agent_t *agent,
                                long long *last_prompt,
                                long long *last_completion,
                                int *last_requests, int *last_retries) {
    if (!session || !agent) return;
    session->prompt_tokens += agent->prompt_tokens - *last_prompt;
    session->completion_tokens += agent->completion_tokens - *last_completion;
    session->request_count += agent->request_count - *last_requests;
    session->retry_count += agent->retry_count - *last_retries;
    *last_prompt = agent->prompt_tokens;
    *last_completion = agent->completion_tokens;
    *last_requests = agent->request_count;
    *last_retries = agent->retry_count;
}

static void print_usage_line(const session_t *session) {
    if (!session || session->request_count <= 0) return;
    fprintf(stderr,
            "[usage] %d request(s)%s, %lld in / %lld out tokens (total %lld)\n",
            session->request_count,
            session->retry_count > 0 ? " (with retries)" : "",
            session->prompt_tokens, session->completion_tokens,
            session->prompt_tokens + session->completion_tokens);
}

/* ── Approval prompts ───────────────────────────────────────────── */

static bool prompt_approval(const char *tool_name, const char *args_json,
                            void *ctx) {
    (void)ctx;
    fprintf(stderr, "\n[cgent] Tool '%s' requires your approval.\n", tool_name);
    fprintf(stderr, "  args: %.200s\n\n", args_json ? args_json : "");
    fprintf(stderr, "Approve? [y/N] ");
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    return buf[0] == 'y' || buf[0] == 'Y';
}

static bool prompt_confirm(const char *question, void *ctx) {
    (void)ctx;
    fprintf(stderr, "\n[cgent] %s\n", question ? question : "");
    fprintf(stderr, "Approve? [y/N] ");
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    return buf[0] == 'y' || buf[0] == 'Y';
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

    /* ── Config subcommand: cgent config <provider> <api_key> ──── */
    if (argc >= 4 && strcmp(argv[1], "config") == 0) {
        int ret = config_set_provider_key(argv[2], argv[3]);
        if (ret < 0) {
            fprintf(stderr, "Error: Failed to write config\n");
            return 1;
        }
        printf("Configured %d model(s) for provider '%s' in ~/.cgent/settings.json\n",
               ret, argv[2]);
        return 0;
    }

    /* ── MCP subcommand: cgent mcp ... ─────────────────────────── */
    if (argc >= 2 && strcmp(argv[1], "mcp") == 0) {
        return mcp_main(argc - 1, argv + 1);
    }

    /* ── Skill subcommand: cgent skill ... ─────────────────────── */
    if (argc >= 2 && strcmp(argv[1], "skill") == 0) {
        return skill_main(argc - 1, argv + 1);
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

    /* Re-resolve model config after CLI may have changed model */
    if (args.model && args.model[0]) {
        config_switch_model(cfg, args.model);
    }

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
        .api_key          = cfg->api_key,
        .base_url         = cfg->base_url ? cfg->base_url : strdup(api->default_base_url),
        .model            = cfg->model ? cfg->model : strdup(api->default_model),
        .temperature      = cfg->temperature,
        .max_tokens       = cfg->max_tokens,
        .stream           = cfg->stream,
        .thinking_enabled   = cfg->thinking_enabled,
        .thinking_configured = cfg->thinking_configured,
        .reasoning_effort   = cfg->reasoning_effort,
        .max_retries        = cfg->max_retries,
        .context_length     = cfg->context_length,
        .auto_compact       = cfg->auto_compact,
        .compact_ratio      = cfg->compact_ratio,
        .parallel_tools     = cfg->parallel_tools,
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

    /* ── Session management ───────────────────────────────────── */
    session_t *session = NULL;
    if (args.resume_uuid && args.resume_uuid[0]) {
        session = session_load(args.resume_uuid);
        if (session) {
            printf("Resumed session: %s (%d messages)\n",
                   session->uuid, session->message_count);
            /* Show messages */
            for (int i = 0; i < session->message_count; i++) {
                message_t *m = &session->messages[i];
                const char *role = "?";
                switch (m->role) {
                case MSG_ROLE_SYSTEM:    role = "system"; break;
                case MSG_ROLE_USER:      role = "user  "; break;
                case MSG_ROLE_ASSISTANT:  role = "assist"; break;
                case MSG_ROLE_TOOL:      role = "tool  "; break;
                }
                const char *preview = m->content ? m->content : "";
                size_t plen = strlen(preview);
                if (plen > 80) plen = 80;
                printf("  [%d] %s: %.*s%s\n", i, role, (int)plen, preview,
                       strlen(preview) > 80 ? "..." : "");
            }
            /* Restore messages into agent */
            for (int i = 0; i < session->message_count; i++)
                agent_add_message(agent, &session->messages[i]);
        } else {
            fprintf(stderr, "Session not found: %s\n", args.resume_uuid);
        }
    }
    if (!session) {
        session = calloc(1, sizeof(session_t));
        session->uuid = session_generate_uuid();
        session->message_cap = 64;
        session->messages = calloc(session->message_cap, sizeof(message_t));
        printf("Session: %s\n", session->uuid);
    }
    if (session && !session->provider) {
        session->provider = strdup(cfg->provider);
        session->model = strdup(cfg->model);
        if (cfg->system_prompt)
            session->system_prompt = strdup(cfg->system_prompt);
    }

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

    /* ── MCP bridge: enable configured MCP servers ─────────────── */
    mcp_bridge_t *mcp_bridges = NULL;
    int mcp_bridge_count = 0;
    {
        mcp_config_t *mcfg = mcp_config_load();
        if (mcfg) {
            int am_count = 0;
            char **am_names = config_resolve_agent_mcp_servers(cfg->agent_dir,
                                                               &am_count);
            int cap = args.mcp_enable_count + am_count + 8;
            const char **enabled = calloc(cap, sizeof(char *));
            int name_count = 0;
            if (enabled) {
                for (int i = 0; i < args.mcp_enable_count; i++)
                    enabled[name_count++] = args.mcp_enable[i];
                for (int i = 0; i < am_count; i++)
                    enabled[name_count++] = am_names[i];
            }

            if (args.mcp_all || name_count > 0) {
                if (cfg->verbose) {
                    fprintf(stderr, "[cgent] Starting MCP servers (%d requested)\n",
                            name_count);
                }
                mcp_bridges_start(mcfg, enabled, name_count, args.mcp_all,
                                  &mcp_bridges, &mcp_bridge_count);
                for (int i = 0; i < mcp_bridge_count; i++) {
                    for (int t = 0; t < mcp_bridges[i].tool_count; t++)
                        agent_add_tool(agent, &mcp_bridges[i].tools[t]);
                }
                if (cfg->verbose) {
                    fprintf(stderr, "[cgent] %d MCP server(s) connected, "
                            "%d tool(s)\n", mcp_bridge_count,
                            agent->n_tools);
                }
            }
            free(enabled);
            for (int i = 0; i < am_count; i++) free(am_names[i]);
            free(am_names);
            mcp_config_free(mcfg);
        }
    }

    int rc = 0;

    /* ── Approval hooks: prompt in interactive REPL; -q/-y auto-allow ── */
    if (!args.yes && !args.query) {
        tool_set_approval_callback(prompt_approval, NULL);
        tool_set_confirm_callback(prompt_confirm, NULL);
    }

    long long last_prompt = 0, last_completion = 0;
    int last_requests = 0, last_retries = 0;

    if (args.query) {
        /* ── Single-shot mode ──────────────────────────────────── */
        /* Add user input to session */
        message_t qmsg = { .role = MSG_ROLE_USER, .content = strdup(args.query) };
        session_add_message(session, &qmsg);
        free(qmsg.content);

        if (cfg->stream) {
            message_t *resp = agent_chat_stream(agent, args.query, on_token, NULL);
            printf("\n"); fflush(stdout);
            /* Add response to session */
            if (resp) session_add_message(session, resp);
            message_free(resp);
        } else {
            message_t *resp = agent_chat(agent, args.query);
            if (resp && resp->content) {
                printf("%s\n", resp->content);
            }
            /* Add response to session */
            if (resp) session_add_message(session, resp);
            message_free(resp);
        }
        session_track_usage(session, agent, &last_prompt, &last_completion,
                            &last_requests, &last_retries);
        print_usage_line(session);
        /* Save session */
        session_save(session, cfg);
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
            /* !cmd — execute bash command directly */
            if (line[0] == '!' && line[1]) {
                printf("%s\n", line + 1);
                int rc;
                char *out = os_exec_capture(line + 1, &rc);
                if (out) { printf("%s", out); free(out); }
                if (rc != 0) printf("(exit: %d)\n", rc);
                free(line);
                continue;
            }
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
                    printf("  /agents       — List installed agents\n");
                    printf("  /context      — Show context usage breakdown\n");
                    printf("  /usage        — Show API token usage for this session\n");
                    printf("  /compact      — Compress conversation history\n");
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
                } else if (strcmp(line, "/agents") == 0) {
                    printf("Installed agents:\n");
                    DIR *d = opendir("agents");
                    if (d) {
                        struct dirent *e;
                        int count = 0;
                        while ((e = readdir(d))) {
                            if (e->d_name[0] == '.') continue;
                            char *ag_path = os_path_join("agents", e->d_name);
                            char *md_path = os_path_join(ag_path, "AGENTS.md");
                            int is_agent = os_path_exists(md_path);
                            const char *mark = (cfg->agent_dir && strstr(cfg->agent_dir, e->d_name)) ? " *" : "  ";
                            printf("%s%s%s\n", mark, e->d_name, is_agent ? "" : " (no AGENTS.md)");
                            if (is_agent) count++;
                            free(md_path);
                            free(ag_path);
                        }
                        printf("Total: %d agent(s)\n", count);
                        closedir(d);
                    } else {
                        printf("  (no agents directory)\n");
                    }
                } else if (strcmp(line, "/context") == 0) {
                    /* Token estimation: ASCII ~4 chars/token, CJK ~1 char/token */
                    agent_context_stats_t st;
                    agent_context_stats(agent, &st);
                    long long sys_tokens  = st.system_tokens;
                    long long tool_tokens = st.tool_tokens;
                    long long msg_tokens  = st.message_tokens;
                    int skill_tokens = 0;
                    if (cfg->skills) {
                        for (int i = 0; i < cfg->skills->count; i++) {
                            if (cfg->skills->skills[i].instruction)
                                skill_tokens += (int)agent_estimate_tokens(
                                    cfg->skills->skills[i].instruction);
                        }
                    }
                    long long total = sys_tokens + tool_tokens + msg_tokens
                                    + skill_tokens;
                    int max_ctx = cfg->context_length;

                    printf("Context Usage (%s, max %d tokens)\n", cfg->model, max_ctx);
                    printf("  System prompt: %8lld tokens (%4.1f%%)\n",
                           sys_tokens, max_ctx > 0 ? 100.0*sys_tokens/max_ctx : 0);
                    printf("  Tools:         %8lld tokens (%4.1f%%)\n",
                           tool_tokens, max_ctx > 0 ? 100.0*tool_tokens/max_ctx : 0);
                    printf("  Skills:        %8d tokens (%4.1f%%)\n",
                           skill_tokens, max_ctx > 0 ? 100.0*skill_tokens/max_ctx : 0);
                    printf("  Messages:      %8lld tokens (%4.1f%%)\n",
                           msg_tokens, max_ctx > 0 ? 100.0*msg_tokens/max_ctx : 0);
                    printf("  ─────────────────────────────\n");
                    printf("  Total:         %8lld tokens (%4.1f%%)\n",
                           total, max_ctx > 0 ? 100.0*total/max_ctx : 0);
                    printf("  Free:          %8lld tokens (%4.1f%%)\n",
                           (long long)max_ctx - total,
                           max_ctx > 0 ? 100.0*(max_ctx-total)/max_ctx : 0);
                    if (session->request_count > 0) {
                        printf("  ─────────────────────────────\n");
                        printf("  API (actual):   %8lld in / %lld out tokens\n",
                               session->prompt_tokens, session->completion_tokens);
                        printf("  Requests:       %8d  (retries: %d)\n",
                               session->request_count, session->retry_count);
                    }
                } else if (strcmp(line, "/usage") == 0) {
                    printf("API usage (this session):\n");
                    printf("  Requests:      %d\n", session->request_count);
                    printf("  Retries:       %d\n", session->retry_count);
                    printf("  Prompt tokens: %lld\n", session->prompt_tokens);
                    printf("  Completion:    %lld\n", session->completion_tokens);
                    printf("  Total tokens:  %lld\n",
                           session->prompt_tokens + session->completion_tokens);
                    if (session->request_count == 0)
                        printf("  (no API requests yet)\n");
                } else if (strcmp(line, "/compact") == 0) {
                    if (agent->n_messages == 0) {
                        printf("Nothing to compact — conversation is empty.\n");
                    } else {
                        agent_context_stats_t st;
                        agent_context_stats(agent, &st);
                        long long before_tokens = st.total_tokens;
                        printf("Compacting conversation (%d messages, ~%lld tokens)...\n",
                               agent->n_messages, before_tokens);
                        printf("Sending compaction request to %s...\n", cfg->model);
                        int compacted = agent_compact(agent);
                        if (compacted > 0) {
                            agent_context_stats(agent, &st);
                            long long after_tokens = st.total_tokens;
                            printf("Compaction complete: %d messages → 1 summary\n",
                                   compacted);
                            printf("  Before: ~%lld tokens → After: ~%lld tokens (%.0f%% reduction)\n",
                                   before_tokens, after_tokens,
                                   before_tokens > 0
                                       ? 100.0 * (before_tokens - after_tokens) / before_tokens
                                       : 0.0);
                        } else {
                            printf("Error: Compaction returned no content. "
                                   "Conversation has been cleared.\n");
                        }
                    }
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
                            agent->provider.thinking_enabled   = cfg->thinking_enabled;
                            agent->provider.thinking_configured = cfg->thinking_configured;
                            agent->provider.max_retries = cfg->max_retries;
                            agent->provider.context_length = cfg->context_length;
                            agent->provider.auto_compact = cfg->auto_compact;
                            agent->provider.compact_ratio = cfg->compact_ratio;
                            agent->provider.parallel_tools = cfg->parallel_tools;
                            free(agent->provider.reasoning_effort);
                            agent->provider.reasoning_effort = cfg->reasoning_effort
                                ? strdup(cfg->reasoning_effort) : NULL;
                            printf("Model changed to: %s (provider: %s)\n",
                                   cfg->model, cfg->provider);
                            config_save_current_model(cfg);
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

                        /* Resolve skill directory from SKILL.md path */
                        char skill_dir[1024] = "";
                        if (sk->path) {
                            snprintf(skill_dir, sizeof(skill_dir), "%s", sk->path);
                            char *last_slash = strrchr(skill_dir, '/');
                            if (last_slash) *last_slash = '\0'; /* Remove SKILL.md */
                        }

                        /* Build prompt combining skill instruction with user params */
                        size_t task_sz = strlen(sk->instruction) + strlen(params) + strlen(skill_dir) + 1024;
                        char *task_buf = malloc(task_sz);
                        if (params[0]) {
                            snprintf(task_buf, task_sz,
                                     "Invoke skill '%s' with input: %s\n\n"
                                     "Skill directory: %s\n"
                                     "Scripts directory: %s/scripts\n\n"
                                     "%s",
                                     sk->name, params,
                                     skill_dir, skill_dir,
                                     sk->instruction);
                        } else {
                            snprintf(task_buf, task_sz,
                                     "Invoke skill '%s'.\n\n"
                                     "Skill directory: %s\n"
                                     "Scripts directory: %s/scripts\n\n"
                                     "%s",
                                     sk->name,
                                     skill_dir, skill_dir,
                                     sk->instruction);
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
                /* Add user input to session */
                message_t umsg = { .role = MSG_ROLE_USER, .content = strdup(line) };
                session_add_message(session, &umsg);
                free(umsg.content);

                if (cfg->stream) {
                    message_t *resp = agent_chat_stream(agent, line, on_token, NULL);
                    printf("\n"); fflush(stdout);
                    if (resp) session_add_message(session, resp);
                    message_free(resp);
                } else {
                    message_t *resp = agent_chat(agent, line);
                    if (resp) {
                        if (resp->content) printf("%s\n", resp->content);
                        if (resp->n_tool_calls > 0)
                            printf("[Used %d tool(s)]\n", resp->n_tool_calls);
                        session_add_message(session, resp);
                        message_free(resp);
                    }
                }
                session_track_usage(session, agent, &last_prompt, &last_completion,
                                    &last_requests, &last_retries);
                print_usage_line(session);
                /* Save session */
                if (session->uuid)
                    session_save(session, cfg);
            }
            free(line);
        }
    }

    /* Cleanup */
    if (session && session->uuid && session->uuid[0]) {
        printf("\nResume: cgent --resume %s\n", session->uuid);
    }
    mcp_bridges_stop(mcp_bridges, mcp_bridge_count);
    agent_free(agent);
    session_free(session);
    config_free(cfg);
    http_cleanup();

    return rc;
}
