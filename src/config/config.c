/*
 * config.c — Configuration loading
 *
 * Priority (lowest to highest):
 *   1. Built-in defaults
 *   2. ~/.cgent/settings.json
 *   3. Environment variables (provider-specific: DEEPSEEK_API_KEY, etc.)
 *   4. AGENTS.md from agent directory
 *   5. CLI arguments (applied separately via config_apply_cli)
 */
#include "config.h"
#include "json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── cgent directory ────────────────────────────────────────────── */

char *config_cgent_dir(void) {
    char *home = os_home_dir();
    char *dir = os_path_join(home, ".cgent");
    free(home);
    /* Create if it doesn't exist */
    if (!os_path_exists(dir)) {
        os_mkdir_p(dir);
    }
    return dir;
}

/* ── Defaults ───────────────────────────────────────────────────── */

static cgent_config_t defaults(void) {
    cgent_config_t cfg = {
        .provider      = strdup("deepseek"),
        .model         = strdup("deepseek-chat"),
        .api_key       = NULL,
        .base_url      = NULL,
        .providers     = {{0}},
        .agent_dir     = strdup("agents/cgent/"),
        .system_prompt = NULL,
        .temperature   = 0.7,
        .max_tokens    = 4096,
        .stream        = true,
        .verbose       = false,
        .config_path   = NULL,
        .cgent_dir     = config_cgent_dir(),
        .mcp_server_commands = NULL,
        .mcp_server_count    = 0,
    };
    return cfg;
}

/* ── settings.json loading ──────────────────────────────────────── */

static void apply_settings_file(cgent_config_t *cfg) {
    char *path = os_path_join(cfg->cgent_dir, "settings.json");
    if (!path || !os_path_exists(path)) {
        free(path);
        return;
    }

    /* Read file */
    FILE *fp = fopen(path, "r");
    if (!fp) { free(path); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(fp); free(path); return; }
    char *data = malloc(sz + 1);
    size_t nread = fread(data, 1, sz, fp);
    data[nread] = '\0';
    fclose(fp);

    json_value_t *root = json_parse(data);
    free(data);
    if (!root) { free(path); return; }

    /* Top-level fields */
    json_value_t *v;

    v = json_object_get(root, "provider");
    if (v && json_is_string(v)) { free(cfg->provider); cfg->provider = strdup(json_string_value(v)); }

    v = json_object_get(root, "model");
    if (v && json_is_string(v)) { free(cfg->model); cfg->model = strdup(json_string_value(v)); }

    v = json_object_get(root, "temperature");
    if (v && json_is_number(v)) cfg->temperature = json_number_value(v);

    v = json_object_get(root, "max_tokens");
    if (v && json_is_number(v)) cfg->max_tokens = (int)json_number_value(v);

    v = json_object_get(root, "stream");
    if (v && json_is_bool(v)) cfg->stream = json_bool_value(v);

    v = json_object_get(root, "agent_dir");
    if (v && json_is_string(v)) { free(cfg->agent_dir); cfg->agent_dir = strdup(json_string_value(v)); }

    /* Provider-specific entries */
    json_value_t *providers = json_object_get(root, "providers");
    if (providers && json_is_object(providers)) {
        static const char *names[] = {"deepseek", "openai", "anthropic"};
        for (int i = 0; i < 3; i++) {
            json_value_t *pe = json_object_get(providers, names[i]);
            if (pe && json_is_object(pe)) {
                v = json_object_get(pe, "api_key");
                if (v && json_is_string(v)) {
                    cfg->providers[i].api_key = strdup(json_string_value(v));
                }
                v = json_object_get(pe, "base_url");
                if (v && json_is_string(v)) {
                    cfg->providers[i].base_url = strdup(json_string_value(v));
                }
            }
        }
    }

    json_free(root);
    free(path);
}

/* ── Environment ────────────────────────────────────────────────── */

static void apply_env(cgent_config_t *cfg) {
    char *val;

    /* Provider-specific API keys (replaces generic CGENT_API_KEY) */
    val = os_getenv("DEEPSEEK_API_KEY");
    if (val) { free(cfg->providers[0].api_key); cfg->providers[0].api_key = val; }

    val = os_getenv("OPENAI_API_KEY");
    if (val) { free(cfg->providers[1].api_key); cfg->providers[1].api_key = val; }

    val = os_getenv("ANTHROPIC_API_KEY");
    if (val) { free(cfg->providers[2].api_key); cfg->providers[2].api_key = val; }

    val = os_getenv("CGENT_PROVIDER");
    if (val) { free(cfg->provider); cfg->provider = val; }

    val = os_getenv("CGENT_MODEL");
    if (val) { free(cfg->model); cfg->model = val; }

    val = os_getenv("CGENT_BASE_URL");
    if (val) { free(cfg->base_url); cfg->base_url = val; }

    val = os_getenv("CGENT_AGENT_DIR");
    if (val) { free(cfg->agent_dir); cfg->agent_dir = val; }

    val = os_getenv("CGENT_TEMPERATURE");
    if (val) { cfg->temperature = atof(val); free(val); }

    val = os_getenv("CGENT_MAX_TOKENS");
    if (val) { cfg->max_tokens = atoi(val); free(val); }
}

/* ── Resolve current provider's api_key and base_url ────────────── */

static void resolve_provider(cgent_config_t *cfg) {
    int idx = 0;
    if (strcmp(cfg->provider, "openai") == 0) idx = 1;
    else if (strcmp(cfg->provider, "anthropic") == 0) idx = 2;

    /* API key: CLI override > env > settings.json */
    /* (cfg->api_key is set by CLI via config_apply_cli) */
    if (!cfg->api_key && cfg->providers[idx].api_key &&
        cfg->providers[idx].api_key[0]) {
        cfg->api_key = strdup(cfg->providers[idx].api_key);
    }

    /* Base URL: CLI override > settings.json > provider default */
    /* (cfg->base_url is set by CLI via config_apply_cli) */
    if (!cfg->base_url && cfg->providers[idx].base_url &&
        cfg->providers[idx].base_url[0]) {
        cfg->base_url = strdup(cfg->providers[idx].base_url);
    }
}

/* ── Agent prompt ────────────────────────────────────────────────── */

char *config_resolve_agent_prompt(const char *agent_dir) {
    char *path = os_path_join(agent_dir, "AGENTS.md");
    if (!path) return NULL;
    if (!os_path_exists(path)) { free(path); return NULL; }

    agent_md_t *am = agent_md_parse(path);
    free(path);
    if (!am) return NULL;

    char *prompt = am->instruction ? strdup(am->instruction) : NULL;
    agent_md_free(am);
    return prompt;
}

/* ── Main config loading ────────────────────────────────────────── */

cgent_config_t *config_load(void) {
    cgent_config_t *cfg = malloc(sizeof(cgent_config_t));
    if (!cfg) return NULL;
    *cfg = defaults();

    /* Layer 2: ~/.cgent/settings.json */
    apply_settings_file(cfg);

    /* Layer 3: environment variables */
    apply_env(cfg);

    /* Layer 4: resolve provider key/url from settings/env */
    resolve_provider(cfg);

    /* Layer 5: AGENTS.md from agent directory */
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
    for (int i = 0; i < 3; i++) {
        free(cfg->providers[i].api_key);
        free(cfg->providers[i].base_url);
    }
    free(cfg->agent_dir);
    free(cfg->system_prompt);
    free(cfg->config_path);
    free(cfg->cgent_dir);
    for (int i = 0; i < cfg->mcp_server_count; i++)
        free(cfg->mcp_server_commands[i]);
    free(cfg->mcp_server_commands);
    free(cfg);
}
