/*
 * config.c — Configuration loading, organized by models
 *
 * settings.json format:
 * {
 *   "default_model": "deepseek-v4-flash",
 *   "models": {
 *     "deepseek-v4-flash": {
 *       "provider": "deepseek",
 *       "api_key": "sk-xxx",
 *       "base_url": "https://api.deepseek.com",
 *       "temperature": 0.7,
 *       "max_tokens": 4096,
 *       "stream": true
 *     },
 *     ...
 *   }
 * }
 */
#include "config.h"
#include "json.h"
#include "platform.h"
#include "skills.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── cgent directory ────────────────────────────────────────────── */

char *config_cgent_dir(void) {
    char *home = os_home_dir();
    char *dir = os_path_join(home, ".cgent");
    free(home);
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    return dir;
}

/* ── Built-in defaults ──────────────────────────────────────────── */

static void add_default_model(cgent_config_t *cfg, const char *name,
                               const char *provider, const char *base_url) {
    if (cfg->model_count >= CGENT_MAX_MODELS) return;
    model_entry_t *m = &cfg->models[cfg->model_count++];
    m->name        = strdup(name);
    m->provider    = strdup(provider);
    m->api_key     = NULL;
    m->base_url    = strdup(base_url);
    m->temperature = 0.7;
    m->max_tokens  = 4096;
    m->stream      = true;
    m->max_retries = 3;
    m->auto_compact = true;
    m->compact_ratio = 0.75;
    m->parallel_tools = true;
    /* Set context length based on provider defaults */
    if (strcmp(provider, "deepseek") == 0)      m->context_length = 1000000;
    else if (strcmp(provider, "openai") == 0)   m->context_length = 128000;
    else if (strcmp(provider, "anthropic") == 0) m->context_length = 200000;
    else                                         m->context_length = 100000;
}

static void add_default_models(cgent_config_t *cfg) {
    add_default_model(cfg, "deepseek-v4-flash",   "deepseek",  "https://api.deepseek.com");
    add_default_model(cfg, "deepseek-v4-pro[1m]", "deepseek",  "https://api.deepseek.com");
    add_default_model(cfg, "gpt-4o",           "openai",    "https://api.openai.com");
    add_default_model(cfg, "gpt-4o-mini",      "openai",    "https://api.openai.com");
    add_default_model(cfg, "claude-sonnet-4-6","anthropic", "https://api.anthropic.com");
    add_default_model(cfg, "claude-opus-4-8",  "anthropic", "https://api.anthropic.com");
}

static cgent_config_t defaults(void) {
    cgent_config_t cfg = {0};

    cfg.active_model  = -1;  /* Will be set after models are loaded */
    cfg.provider      = strdup("deepseek");
    cfg.model         = strdup("deepseek-v4-flash");
    cfg.api_key       = NULL;
    cfg.base_url      = NULL;  /* Will be set from active model */
    cfg.temperature    = 0.7;
    cfg.max_tokens     = 4096;
    cfg.context_length = 1000000;
    cfg.stream         = true;
    cfg.agent_dir     = strdup("agents/cgent/");
    cfg.system_prompt = NULL;
    cfg.verbose       = false;
    cfg.max_retries   = 3;
    cfg.auto_compact  = true;
    cfg.compact_ratio = 0.75;
    cfg.parallel_tools = true;
    cfg.cgent_dir     = config_cgent_dir();

    return cfg;
}

/* ── settings.json loading ──────────────────────────────────────── */

static void apply_settings_file(cgent_config_t *cfg) {
    char *path = os_path_join(cfg->cgent_dir, "settings.json");
    if (!path || !os_path_exists(path)) { free(path); return; }

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

    /* current_model takes priority over default_model */
    json_value_t *curm = json_object_get(root, "current_model");
    const char *current_name = curm && json_is_string(curm) ? json_string_value(curm) : NULL;
    json_value_t *defm = json_object_get(root, "default_model");
    const char *default_name = defm && json_is_string(defm) ? json_string_value(defm) : NULL;
    const char *active_name = current_name ? current_name : default_name;

    /* models section */
    json_value_t *models_obj = json_object_get(root, "models");
    if (models_obj && json_is_object(models_obj)) {
        /* Iterate over model entries */
        json_iter_t it = json_iter_object(models_obj);
        const char *key;
        json_value_t *val;
        while (json_iter_next(&it, &key, &val)) {
            if (!val || !json_is_object(val)) continue;

            /* Find or create model entry */
            int idx = -1;
            for (int i = 0; i < cfg->model_count; i++) {
                if (strcmp(cfg->models[i].name, key) == 0) { idx = i; break; }
            }
            if (idx < 0 && cfg->model_count < CGENT_MAX_MODELS) {
                idx = cfg->model_count++;
                cfg->models[idx].name = strdup(key);
                cfg->models[idx].provider = strdup("deepseek"); /* default */
            }
            if (idx < 0) continue;

            model_entry_t *m = &cfg->models[idx];
            json_value_t *v;

            v = json_object_get(val, "provider");
            if (v && json_is_string(v)) { free(m->provider); m->provider = strdup(json_string_value(v)); }

            v = json_object_get(val, "api_key");
            if (v && json_is_string(v) && json_string_value(v)[0]) {
                free(m->api_key); m->api_key = strdup(json_string_value(v));
            }

            v = json_object_get(val, "base_url");
            if (v && json_is_string(v) && json_string_value(v)[0]) {
                free(m->base_url); m->base_url = strdup(json_string_value(v));
            }

            v = json_object_get(val, "temperature");
            if (v && json_is_number(v)) m->temperature = json_number_value(v);

            v = json_object_get(val, "max_tokens");
            if (v && json_is_number(v)) m->max_tokens = (int)json_number_value(v);

            v = json_object_get(val, "stream");
            if (v && json_is_bool(v)) m->stream = json_bool_value(v);

            v = json_object_get(val, "context_length");
            if (v && json_is_number(v)) m->context_length = (int)json_number_value(v);

            v = json_object_get(val, "max_retries");
            if (v && json_is_number(v)) {
                int r = (int)json_number_value(v);
                m->max_retries = r >= 0 ? r : 0;
            }

            v = json_object_get(val, "auto_compact");
            if (v && json_is_bool(v)) m->auto_compact = json_bool_value(v);

            v = json_object_get(val, "compact_ratio");
            if (v && json_is_number(v)) {
                double r = json_number_value(v);
                m->compact_ratio = r > 0 && r < 1 ? r : 0.75;
            }

            v = json_object_get(val, "parallel_tools");
            if (v && json_is_bool(v)) m->parallel_tools = json_bool_value(v);

            v = json_object_get(val, "response_format");
            if (v && json_is_string(v)) {
                free(m->response_format);
                m->response_format = strdup(json_string_value(v));
            }
            v = json_object_get(val, "json_schema");
            if (v && json_is_string(v)) {
                free(m->json_schema);
                m->json_schema = strdup(json_string_value(v));
            }

            /* ── thinking: {"type": "enabled"/"disabled"} ── */
            json_value_t *thinking = json_object_get(val, "thinking");
            if (thinking && json_is_object(thinking)) {
                v = json_object_get(thinking, "type");
                if (v && json_is_string(v)) {
                    m->thinking_configured = true;
                    m->thinking_enabled = (strcmp(json_string_value(v), "enabled") == 0);
                }
            }

            /* ── reasoning_effort: "low"/"medium"/"high"/"max" ── */
            v = json_object_get(val, "reasoning_effort");
            if (v && json_is_string(v)) {
                free(m->reasoning_effort);
                m->reasoning_effort = strdup(json_string_value(v));
            }

            /* ── output_config: {"effort": "high"/"max"} (alt format) ── */
            json_value_t *oc = json_object_get(val, "output_config");
            if (oc && json_is_object(oc)) {
                v = json_object_get(oc, "effort");
                if (v && json_is_string(v) && !m->reasoning_effort) {
                    m->reasoning_effort = strdup(json_string_value(v));
                }
            }
        }
    }

    /* If settings.json defined models, use only those.
     * Otherwise, add built-in defaults so the agent still works. */
    int models_from_settings = (models_obj && json_is_object(models_obj)
                                && json_object_size(models_obj) > 0);

    if (!models_from_settings) {
        add_default_models(cfg);
    }

    /* Set active model from current_model or default_model */
    if (active_name) {
        for (int i = 0; i < cfg->model_count; i++) {
            if (strcmp(cfg->models[i].name, active_name) == 0) {
                cfg->active_model = i;
                break;
            }
        }
    }

    /* Top-level overrides (for backward compat) */
    json_value_t *v;
    v = json_object_get(root, "agent_dir");
    if (v && json_is_string(v)) { free(cfg->agent_dir); cfg->agent_dir = strdup(json_string_value(v)); }

    json_free(root);
    free(path);
}

/* ── Environment ────────────────────────────────────────────────── */

static void apply_env(cgent_config_t *cfg) {
    char *val;

    /* CGENT_API_KEY applies to all models */
    val = os_getenv("CGENT_API_KEY");
    if (val) {
        for (int i = 0; i < cfg->model_count; i++) {
            if (!cfg->models[i].api_key) {
                cfg->models[i].api_key = strdup(val);
            }
        }
        free(val);
    }

    val = os_getenv("CGENT_AGENT_DIR");
    if (val) { free(cfg->agent_dir); cfg->agent_dir = val; }

    val = os_getenv("CGENT_TEMPERATURE");
    if (val) { cfg->temperature = atof(val); free(val); }

    val = os_getenv("CGENT_MAX_TOKENS");
    if (val) { cfg->max_tokens = atoi(val); free(val); }
}

/* ── Resolve active model into flat config fields ────────────────── */

static void resolve_active_model(cgent_config_t *cfg) {
    if (cfg->active_model < 0 || cfg->active_model >= cfg->model_count) {
        cfg->active_model = cfg->model_count > 0 ? 0 : -1;
    }
    if (cfg->active_model < 0) return;  /* No models available */

    model_entry_t *m = &cfg->models[cfg->active_model];

    free(cfg->provider);  cfg->provider  = strdup(m->provider);
    free(cfg->model);     cfg->model     = strdup(m->name);
    cfg->temperature    = m->temperature;
    cfg->max_tokens     = m->max_tokens;
    cfg->context_length   = m->context_length;
    cfg->stream           = m->stream;
    cfg->thinking_enabled   = m->thinking_enabled;
    cfg->thinking_configured = m->thinking_configured;
    cfg->max_retries      = m->max_retries;
    cfg->auto_compact     = m->auto_compact;
    cfg->compact_ratio    = m->compact_ratio;
    cfg->parallel_tools   = m->parallel_tools;
    free(cfg->response_format);
    cfg->response_format  = m->response_format ? strdup(m->response_format) : NULL;
    free(cfg->json_schema);
    cfg->json_schema      = m->json_schema ? strdup(m->json_schema) : NULL;
    free(cfg->reasoning_effort);
    cfg->reasoning_effort = m->reasoning_effort ? strdup(m->reasoning_effort) : NULL;

    /* api_key: keep CLI override if set, else use model's */
    if (!cfg->api_key) {
        cfg->api_key = m->api_key ? strdup(m->api_key) : NULL;
    }
    /* base_url: always use model's (CLI overrides applied later via config_apply_cli) */
    free(cfg->base_url);
    cfg->base_url = m->base_url ? strdup(m->base_url) : NULL;
}

/* ── Model switching ────────────────────────────────────────────── */

int config_find_model(cgent_config_t *cfg, const char *name) {
    for (int i = 0; i < cfg->model_count; i++) {
        if (strcmp(cfg->models[i].name, name) == 0) return i;
    }
    return -1;
}

int config_switch_model(cgent_config_t *cfg, const char *model_name) {
    int idx = config_find_model(cfg, model_name);
    if (idx < 0) return -1;
    cfg->active_model = idx;

    /* Clear overridden fields so resolve picks up new model */
    free(cfg->api_key);  cfg->api_key = NULL;
    free(cfg->base_url); cfg->base_url = NULL;

    resolve_active_model(cfg);
    return 0;
}

/* ── Save current model ─────────────────────────────────────────── */

void config_save_current_model(const cgent_config_t *cfg) {
    if (!cfg || !cfg->model) return;
    char *path = os_path_join(cfg->cgent_dir, "settings.json");
    if (!path || !os_path_exists(path)) { free(path); return; }

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

    /* Update current_model */
    json_value_t *cur = json_object_get(root, "current_model");
    if (cur && json_is_string(cur)) {
        /* Replace existing value */
        json_object_del(root, "current_model");
    }
    json_object_set(root, "current_model", json_string(cfg->model));

    char *json_str = json_stringify_pretty(root);
    json_free(root);

    fp = fopen(path, "w");
    if (fp) { fputs(json_str, fp); fclose(fp); }
    free(json_str);
    free(path);
}

/* ── Agent prompt ────────────────────────────────────────────────── */

/* ── Provider model presets ─────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *provider;
    const char *base_url;
    double temperature;
    int max_tokens;
    int context_length;
    bool stream;
    bool thinking_enabled;
    const char *reasoning_effort;
    int max_retries;
    bool auto_compact;
    double compact_ratio;
} model_preset_t;

static const model_preset_t PROVIDER_PRESETS[] = {
    /* DeepSeek models */
    { "deepseek-v4-flash",   "deepseek", "https://api.deepseek.com",
      0.7, 32768, 1048576, true, true,  "high", 3, true, 0.75 },
    { "deepseek-v4-pro[1m]", "deepseek", "https://api.deepseek.com",
      0.7, 32768, 1048576, true, true,  "high", 3, true, 0.75 },
    /* OpenAI models */
    { "gpt-4o",              "openai",   "https://api.openai.com",
      0.7, 4096,  128000,  true,  false, NULL, 3, true, 0.75 },
    { "gpt-4o-mini",         "openai",   "https://api.openai.com",
      0.7, 4096,  128000,  true,  false, NULL, 3, true, 0.75 },
    /* Anthropic models */
    { "claude-sonnet-4-6",   "anthropic","https://api.anthropic.com",
      0.7, 4096,  200000,  true,  false, NULL, 3, true, 0.75 },
    { "claude-opus-4-8",     "anthropic","https://api.anthropic.com",
      0.7, 4096,  200000,  true,  false, NULL, 3, true, 0.75 },
    { NULL, NULL, NULL, 0, 0, 0, false, false, NULL, 0, false, 0 },
};

/* Apply a model preset to a JSON object (for settings.json "models" section) */
static void apply_preset_to_json(json_value_t *model_obj, const model_preset_t *p,
                                  const char *api_key) {
    json_object_set(model_obj, "provider", json_string(p->provider));
    if (api_key && api_key[0])
        json_object_set(model_obj, "api_key", json_string(api_key));
    json_object_set(model_obj, "base_url", json_string(p->base_url));
    json_object_set(model_obj, "temperature", json_number(p->temperature));
    json_object_set(model_obj, "max_tokens", json_number(p->max_tokens));
    json_object_set(model_obj, "context_length", json_number(p->context_length));
    json_object_set(model_obj, "stream", json_bool(p->stream));
    json_object_set(model_obj, "max_retries", json_number(p->max_retries));
    json_object_set(model_obj, "auto_compact", json_bool(p->auto_compact));
    json_object_set(model_obj, "compact_ratio", json_number(p->compact_ratio));
    if (p->thinking_enabled) {
        json_value_t *thinking = json_object();
        json_object_set(thinking, "type", json_string("enabled"));
        json_object_set(model_obj, "thinking", thinking);
    }
    if (p->reasoning_effort)
        json_object_set(model_obj, "reasoning_effort", json_string(p->reasoning_effort));
}

/* Configure API key for all models of a provider in settings.json.
 * Adds known models for the provider if they don't exist.
 * Returns the number of models configured, or -1 on error. */
int config_set_provider_key(const char *provider, const char *api_key) {
    if (!provider || !api_key) return -1;

    /* Resolve settings.json path */
    char *cgent_dir = config_cgent_dir();
    char *path = os_path_join(cgent_dir, "settings.json");
    free(cgent_dir);

    /* Read existing settings, or start fresh */
    json_value_t *root = NULL;
    if (os_path_exists(path)) {
        FILE *fp = fopen(path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0 && sz <= 65536) {
                char *data = malloc(sz + 1);
                size_t nread = fread(data, 1, sz, fp);
                data[nread] = '\0';
                root = json_parse(data);
                free(data);
            }
            fclose(fp);
        }
    }
    if (!root) root = json_object();

    /* Ensure "models" object exists */
    json_value_t *models_obj = json_object_get(root, "models");
    if (!models_obj || !json_is_object(models_obj)) {
        models_obj = json_object();
        json_object_set(root, "models", models_obj);
    }

    int count = 0;

    /* Step 1: Set API key on ALL existing models matching this provider */
    json_iter_t it = json_iter_object(models_obj);
    const char *key;
    json_value_t *val;
    while (json_iter_next(&it, &key, &val)) {
        if (!val || !json_is_object(val)) continue;
        json_value_t *pv = json_object_get(val, "provider");
        if (pv && json_is_string(pv) && strcmp(json_string_value(pv), provider) == 0) {
            /* Set/update the api_key */
            json_object_del(val, "api_key");
            json_object_set(val, "api_key", json_string(api_key));
            count++;
        }
    }

    /* Step 2: Add known presets for this provider that don't yet exist */
    for (const model_preset_t *p = PROVIDER_PRESETS; p->name; p++) {
        if (strcmp(p->provider, provider) != 0) continue;
        /* Skip if model already exists */
        if (json_object_get(models_obj, p->name)) continue;

        json_value_t *model_obj = json_object();
        apply_preset_to_json(model_obj, p, api_key);
        json_object_set(models_obj, p->name, model_obj);
        count++;
    }

    /* Write back */
    char *json_str = json_stringify_pretty(root);
    json_free(root);

    FILE *fp = fopen(path, "w");
    if (!fp) { free(path); free(json_str); return -1; }
    fputs(json_str, fp);
    fclose(fp);
    free(json_str);
    free(path);

    return count;
}

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

char **config_resolve_agent_mcp_servers(const char *agent_dir, int *count) {
    *count = 0;
    if (!agent_dir) return NULL;
    char *path = os_path_join(agent_dir, "AGENTS.md");
    if (!path) return NULL;
    if (!os_path_exists(path)) { free(path); return NULL; }
    agent_md_t *am = agent_md_parse(path);
    free(path);
    if (!am) return NULL;

    char **names = NULL;
    if (am->mcp_servers_count > 0) {
        names = calloc(am->mcp_servers_count, sizeof(char *));
        if (names) {
            for (int i = 0; i < am->mcp_servers_count; i++)
                names[i] = strdup(am->mcp_servers[i]);
            *count = am->mcp_servers_count;
        }
    }
    agent_md_free(am);
    return names;
}

/* ── Main config loading ────────────────────────────────────────── */

cgent_config_t *config_load(void) {
    cgent_config_t *cfg = malloc(sizeof(cgent_config_t));
    if (!cfg) return NULL;
    *cfg = defaults();

    apply_settings_file(cfg);
    apply_env(cfg);
    resolve_active_model(cfg);

    char *prompt = config_resolve_agent_prompt(cfg->agent_dir);
    if (prompt) { free(cfg->system_prompt); cfg->system_prompt = prompt; }

    /* Load skills from ~/.cgent/skills/ */
    char *skills_dir = os_path_join(cfg->cgent_dir, "skills");
    cfg->skills = skills_load_directory(skills_dir);
    free(skills_dir);

    if (cfg->skills && cfg->skills->count > 0 && cfg->system_prompt) {
        char *enhanced = skills_build_prompt(cfg->skills, cfg->system_prompt);
        if (enhanced) {
            free(cfg->system_prompt);
            cfg->system_prompt = enhanced;
        }
    }

    return cfg;
}

void config_free(cgent_config_t *cfg) {
    if (!cfg) return;
    for (int i = 0; i < cfg->model_count; i++) {
        free(cfg->models[i].name);
        free(cfg->models[i].provider);
        free(cfg->models[i].api_key);
        free(cfg->models[i].base_url);
        free(cfg->models[i].response_format);
        free(cfg->models[i].json_schema);
        free(cfg->models[i].reasoning_effort);
    }
    free(cfg->provider);
    free(cfg->model);
    free(cfg->api_key);
    free(cfg->base_url);
    free(cfg->response_format);
    free(cfg->json_schema);
    free(cfg->reasoning_effort);
    free(cfg->agent_dir);
    free(cfg->system_prompt);
    free(cfg->config_path);
    free(cfg->cgent_dir);
    skills_free(cfg->skills);
    for (int i = 0; i < cfg->mcp_server_count; i++)
        free(cfg->mcp_server_commands[i]);
    free(cfg->mcp_server_commands);
    free(cfg);
}
