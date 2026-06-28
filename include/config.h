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
    char *instruction;          /* Main body after frontmatter */
    char **mcp_servers;         /* MCP server names */
    int mcp_servers_count;
    char **skills;              /* Skill names */
    int skills_count;
} agent_md_t;

/* Parse an AGENTS.md file. Returns NULL on error. */
agent_md_t  *agent_md_parse(const char *filepath);
void         agent_md_free(agent_md_t *am);

/* ── Runtime configuration ──────────────────────────────────────── */

/* Per-provider configuration in settings.json */
typedef struct {
    char *api_key;
    char *base_url;
} provider_entry_t;

typedef struct {
    /* Provider */
    char *provider;             /* "deepseek", "openai", "anthropic" */
    char *model;
    char *api_key;              /* Resolved API key for current provider */
    char *base_url;             /* Resolved base URL */

    /* Provider-specific entries from settings.json */
    provider_entry_t providers[3]; /* [0]=deepseek, [1]=openai, [2]=anthropic */

    /* Agent settings */
    char *agent_dir;            /* Agent directory (e.g. "agents/cgent/") */
    char *system_prompt;        /* Resolved system prompt from AGENTS.md */
    double temperature;
    int max_tokens;
    bool stream;
    bool verbose;

    /* Files */
    char *config_path;          /* Explicit config path override */
    char *cgent_dir;            /* ~/.cgent/ directory */

    /* MCP */
    char **mcp_server_commands;
    int mcp_server_count;
} cgent_config_t;

/* Load config from hierarchy:
 *   1. Built-in defaults
 *   2. ~/.cgent/settings.json
 *   3. Environment variables (DEEPSEEK_API_KEY, OPENAI_API_KEY, ANTHROPIC_API_KEY)
 *   4. AGENTS.md from agent directory (agents/cgent/ by default)
 *   5. CLI arguments override
 *
 * Caller must free with config_free().
 */
cgent_config_t *config_load(void);
void            config_free(cgent_config_t *config);

/* Resolve the system prompt from an agent directory's AGENTS.md */
char *config_resolve_agent_prompt(const char *agent_dir);

/* Get the ~/.cgent directory path (creates it if needed) */
char *config_cgent_dir(void);

/* ── CLI argument parsing ───────────────────────────────────────── */

typedef struct {
    char *provider;
    char *model;
    char *api_key;
    char *base_url;
    char *query;
    char *agent_dir;            /* --agent: override agent directory */
    char *config_path;
    double temperature;
    int max_tokens;
    bool stream;
    bool verbose;
    bool help;
    bool version;
} cli_args_t;

/* Parse command line arguments. Exits on parse errors. */
cli_args_t cli_parse(int argc, char **argv);

/* Merge CLI args into config (CLI takes highest priority) */
void config_apply_cli(cgent_config_t *cfg, const cli_args_t *args);

#endif /* CONFIG_H */
