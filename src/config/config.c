/*
 * config.c — Configuration loading, organized by models
 *
 * settings.json format:
 * {
 *   "default_model": "deepseek-chat",
 *   "models": {
 *     "deepseek-chat": {
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
}

static void add_default_models(cgent_config_t *cfg) {
    add_default_model(cfg, "deepseek-chat",    "deepseek",  "https://api.deepseek.com");
    add_default_model(cfg, "deepseek-reasoner","deepseek",  "https://api.deepseek.com");
    add_default_model(cfg, "gpt-4o",           "openai",    "https://api.openai.com");
    add_default_model(cfg, "gpt-4o-mini",      "openai",    "https://api.openai.com");
    add_default_model(cfg, "claude-sonnet-4-6","anthropic", "https://api.anthropic.com");
    add_default_model(cfg, "claude-opus-4-8",  "anthropic", "https://api.anthropic.com");
}

static cgent_config_t defaults(void) {
    cgent_config_t cfg = {0};

    cfg.active_model  = -1;  /* Will be set after models are loaded */
    cfg.provider      = strdup("deepseek");
    cfg.model         = strdup("deepseek-chat");
    cfg.api_key       = NULL;
    cfg.base_url      = strdup("https://api.deepseek.com");
    cfg.temperature   = 0.7;
    cfg.max_tokens    = 4096;
    cfg.stream        = true;
    cfg.agent_dir     = strdup("agents/cgent/");
    cfg.system_prompt = NULL;
    cfg.verbose       = false;
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

    /* default_model */
    json_value_t *defm = json_object_get(root, "default_model");
    const char *default_name = defm && json_is_string(defm) ? json_string_value(defm) : NULL;

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
        }
    }

    /* If settings.json defined models, use only those.
     * Otherwise, add built-in defaults so the agent still works. */
    int models_from_settings = (models_obj && json_is_object(models_obj)
                                && json_object_size(models_obj) > 0);

    if (!models_from_settings) {
        add_default_models(cfg);
    }

    /* Set active model from default_model or first */
    if (default_name) {
        for (int i = 0; i < cfg->model_count; i++) {
            if (strcmp(cfg->models[i].name, default_name) == 0) {
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
    cfg->temperature = m->temperature;
    cfg->max_tokens  = m->max_tokens;
    cfg->stream      = m->stream;

    /* api_key: keep CLI override if set, else use model's */
    if (!cfg->api_key) {
        cfg->api_key = m->api_key ? strdup(m->api_key) : NULL;
    }
    /* base_url: keep CLI override if set, else use model's */
    if (!cfg->base_url) {
        cfg->base_url = m->base_url ? strdup(m->base_url) : NULL;
    }
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
    }
    free(cfg->provider);
    free(cfg->model);
    free(cfg->api_key);
    free(cfg->base_url);
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
