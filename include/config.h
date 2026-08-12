/*
 * config.h — Configuration loading, CLI args, AGENTS.md parsing
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "skills.h"

/* ── AGENTS.md parsed content ──────────────────────────────────── */

typedef struct {
    char *name;
    char *description;
    char *model;
    char *instruction;
    char **mcp_servers;
    int mcp_servers_count;
    char **skills;
    int skills_count;
} agent_md_t;

agent_md_t  *agent_md_parse(const char *filepath);
void         agent_md_free(agent_md_t *am);

/* ── Model entry (from settings.json models section) ────────────── */

#define CGENT_MAX_MODELS 64

typedef struct {
    char *name;             /* Model name (key), e.g. "deepseek-v4-flash" */
    char *provider;         /* "deepseek", "openai", "anthropic" */
    char *api_key;
    char *base_url;
    double temperature;
    int max_tokens;         /* Max output tokens */
    int context_length;     /* Max context window size */
    bool stream;
    bool thinking_enabled;   /* Deep thinking / chain-of-thought */
    bool thinking_configured; /* Whether thinking was explicitly set */
    char *reasoning_effort;  /* "low", "medium", "high", "max" or NULL */
    int max_retries;         /* Transient-failure retries per request */
    bool auto_compact;       /* Auto-compact when context is nearly full */
    double compact_ratio;    /* Fraction of context that triggers compaction */
} model_entry_t;

/* ── Runtime configuration ──────────────────────────────────────── */

typedef struct {
    /* Known models (from settings.json) */
    model_entry_t models[CGENT_MAX_MODELS];
    int model_count;

    /* Active model index */
    int active_model;

    /* Resolved values from active model (convenience accessors) */
    char *provider;
    char *model;
    char *api_key;
    char *base_url;
    double temperature;
    int max_tokens;
    int context_length;
    bool stream;
    bool thinking_enabled;
    bool thinking_configured;
    char *reasoning_effort;
    int max_retries;
    bool auto_compact;
    double compact_ratio;

    /* Agent settings */
    char *agent_dir;
    char *system_prompt;

    /* General */
    bool verbose;

    /* Files */
    char *config_path;
    char *cgent_dir;

    /* Skills */
    skill_list_t *skills;

    /* MCP */
    char **mcp_server_commands;
    int mcp_server_count;
} cgent_config_t;

/* Load config from hierarchy:
 *   1. Built-in defaults (deepseek-v4-flash model)
 *   2. ~/.cgent/settings.json (models section)
 *   3. Environment variables (DEEPSEEK_API_KEY, etc. override per-model keys)
 *   4. AGENTS.md from agent directory
 *   5. CLI arguments override
 */
cgent_config_t *config_load(void);
void            config_free(cgent_config_t *config);

/* Resolve system prompt from agent directory */
char *config_resolve_agent_prompt(const char *agent_dir);

/* Resolve the mcp_servers list from AGENTS.md in the agent directory.
 * Returns a malloc'd array of names (caller frees each + array), or
 * NULL if the agent directory has no AGENTS.md or no mcp_servers. */
char **config_resolve_agent_mcp_servers(const char *agent_dir, int *count);

/* Get/create ~/.cgent directory */
char *config_cgent_dir(void);

/* Save the current model back to settings.json */
void config_save_current_model(const cgent_config_t *cfg);

/* Configure API key for all models of a provider in settings.json.
 * Adds known models for the provider if they don't exist.
 * Returns the number of models configured, or -1 on error. */
int  config_set_provider_key(const char *provider, const char *api_key);

/* Switch active model by name. Returns 0 on success, -1 if not found.
 * Updates all resolved fields (provider, api_key, base_url, etc.). */
int  config_switch_model(cgent_config_t *cfg, const char *model_name);

/* Find model by name, returns index or -1 */
int  config_find_model(cgent_config_t *cfg, const char *name);

/* ── CLI arguments ──────────────────────────────────────────────── */

typedef struct {
    char *provider;
    char *model;
    char *api_key;
    char *base_url;
    char *query;
    char *agent_dir;
    char *config_path;
    char *resume_uuid;          /* --resume: restore session */
    double temperature;
    int max_tokens;
    int retries;                /* --retries: -1 = unset (use config) */
    bool stream;
    bool no_auto_compact;       /* --no-auto-compact */
    bool yes;                   /* -y/--yes: skip approval prompts */
    bool verbose;
    bool help;
    bool version;

    /* MCP servers to enable for this run (repeatable --mcp) */
    char **mcp_enable;
    int mcp_enable_count;
    bool mcp_all;               /* --mcp-all: start every configured server */
} cli_args_t;

cli_args_t cli_parse(int argc, char **argv);
void config_apply_cli(cgent_config_t *cfg, const cli_args_t *args);

#endif /* CONFIG_H */
