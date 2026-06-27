/*
 * config.c — Configuration loading
 *
 * Priority (lowest to highest):
 *   1. Built-in defaults
 *   2. Environment variables
 *   3. Config file
 *   4. AGENTS.md from agent directory
 *   5. CLI arguments (applied separately via config_apply_cli)
 */
#include "config.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cgent_config_t defaults(void) {
    cgent_config_t cfg = {
        .provider      = strdup("deepseek"),
        .model         = strdup("deepseek-chat"),
        .api_key       = NULL,
        .base_url      = NULL,
        .agent_dir     = strdup("agents/cgent/"),
        .system_prompt = NULL,
        .temperature   = 0.7,
        .max_tokens    = 4096,
        .stream        = true,
        .verbose       = false,
        .config_path   = NULL,
        .mcp_server_commands = NULL,
        .mcp_server_count    = 0,
    };
    return cfg;
}

static void apply_env(cgent_config_t *cfg) {
    char *val;

    val = os_getenv("CGENT_API_KEY");
    if (!val) val = os_getenv("DEEPSEEK_API_KEY");
    if (!val) val = os_getenv("OPENAI_API_KEY");
    if (!val) val = os_getenv("ANTHROPIC_API_KEY");
    if (val) { free(cfg->api_key); cfg->api_key = val; }

    val = os_getenv("CGENT_PROVIDER");
    if (val) { free(cfg->provider); cfg->provider = val; }

    val = os_getenv("CGENT_MODEL");
    if (val) { free(cfg->model); cfg->model = val; }

    val = os_getenv("CGENT_BASE_URL");
    if (val) { free(cfg->base_url); cfg->base_url = val; }

    val = os_getenv("CGENT_AGENT_DIR");
    if (val) { free(cfg->agent_dir); cfg->agent_dir = val; }

    val = os_getenv("CGENT_CONFIG");
    if (val) { free(cfg->config_path); cfg->config_path = val; }

    val = os_getenv("CGENT_TEMPERATURE");
    if (val) { cfg->temperature = atof(val); free(val); }

    val = os_getenv("CGENT_MAX_TOKENS");
    if (val) { cfg->max_tokens = atoi(val); free(val); }
}

/* Resolve the system prompt from an agent directory.
 * Looks for <agent_dir>/AGENTS.md.
 * Returns a malloc'd string, or NULL if not found. */
char *config_resolve_agent_prompt(const char *agent_dir) {
    char *path = os_path_join(agent_dir, "AGENTS.md");
    if (!path) return NULL;
    if (!os_path_exists(path)) {
        free(path);
        return NULL;
    }

    agent_md_t *am = agent_md_parse(path);
    free(path);
    if (!am) return NULL;

    /* Use instruction as system prompt */
    char *prompt = am->instruction ? strdup(am->instruction) : NULL;

    /* If agent.md specifies a model, note it (but caller decides) */
    if (am->model && prompt) {
        /* We could update cfg->model here but let caller handle it */
    }

    agent_md_free(am);
    return prompt;
}

cgent_config_t *config_load(void) {
    cgent_config_t *cfg = malloc(sizeof(cgent_config_t));
    if (!cfg) return NULL;
    *cfg = defaults();

    /* Layer 2: environment */
    apply_env(cfg);

    /* Layer 3: ~/.config/cgent/config.yaml (TBD) */

    /* Layer 4: AGENTS.md from agent directory */
    char *prompt = config_resolve_agent_prompt(cfg->agent_dir);
    if (prompt) {
        free(cfg->system_prompt);
        cfg->system_prompt = prompt;
    }

    return cfg;
}

void config_free(cgent_config_t *cfg) {
    if (!cfg) return;
    free(cfg->provider);
    free(cfg->model);
    free(cfg->api_key);
    free(cfg->base_url);
    free(cfg->agent_dir);
    free(cfg->system_prompt);
    free(cfg->config_path);
    for (int i = 0; i < cfg->mcp_server_count; i++)
        free(cfg->mcp_server_commands[i]);
    free(cfg->mcp_server_commands);
    free(cfg);
}
