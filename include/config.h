/*
 * config.h — Configuration loading, CLI args, AGENTS.md parsing
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

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
    char *name;             /* Model name (key), e.g. "deepseek-chat" */
    char *provider;         /* "deepseek", "openai", "anthropic" */
    char *api_key;
    char *base_url;
    double temperature;
    int max_tokens;
    bool stream;
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
    bool stream;

    /* Agent settings */
    char *agent_dir;
    char *system_prompt;

    /* General */
    bool verbose;

    /* Files */
    char *config_path;
    char *cgent_dir;

    /* MCP */
    char **mcp_server_commands;
    int mcp_server_count;
} cgent_config_t;

/* Load config from hierarchy:
 *   1. Built-in defaults (deepseek-chat model)
 *   2. ~/.cgent/settings.json (models section)
 *   3. Environment variables (DEEPSEEK_API_KEY, etc. override per-model keys)
 *   4. AGENTS.md from agent directory
 *   5. CLI arguments override
 */
cgent_config_t *config_load(void);
void            config_free(cgent_config_t *config);

/* Resolve system prompt from agent directory */
char *config_resolve_agent_prompt(const char *agent_dir);

/* Get/create ~/.cgent directory */
char *config_cgent_dir(void);

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
    double temperature;
    int max_tokens;
    bool stream;
    bool verbose;
    bool help;
    bool version;
} cli_args_t;

cli_args_t cli_parse(int argc, char **argv);
void config_apply_cli(cgent_config_t *cfg, const cli_args_t *args);

#endif /* CONFIG_H */
