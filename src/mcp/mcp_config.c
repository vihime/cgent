/*
 * mcp_config.c — MCP server configuration persistence
 *
 * Stored in ~/.cgent/mcp.json:
 * {
 *   "servers": {
 *     "name": {
 *       "command": "cmd",
 *       "args": ["a", "b"],
 *       "env": {"K": "V"},
 *       "cwd": "/path"
 *     }
 *   }
 * }
 */
#include "mcp.h"
#include "json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Path helpers ───────────────────────────────────────────────── */

static char *mcp_config_path(void) {
    char *home = os_home_dir();
    if (!home) return NULL;
    char *dir = os_path_join(home, ".cgent");
    free(home);
    if (!dir) return NULL;
    if (!os_path_exists(dir)) os_mkdir_p(dir);
    char *path = os_path_join(dir, "mcp.json");
    free(dir);
    return path;
}

/* ── Server lifecycle ───────────────────────────────────────────── */

static mcp_server_t *mcp_server_new(const char *name) {
    mcp_server_t *s = calloc(1, sizeof(mcp_server_t));
    if (!s) return NULL;
    s->name = strdup(name);
    if (!s->name) { free(s); return NULL; }
    return s;
}

static void mcp_server_clear(mcp_server_t *s) {
    if (!s) return;
    free(s->name);
    free(s->command);
    for (int i = 0; i < s->arg_count; i++) free(s->args[i]);
    free(s->args);
    for (int i = 0; i < s->env_count; i++) {
        free(s->env_keys[i]);
        free(s->env_values[i]);
    }
    free(s->env_keys);
    free(s->env_values);
    free(s->cwd);
    memset(s, 0, sizeof(*s));
}

static void mcp_server_free(mcp_server_t *s) {
    if (!s) return;
    mcp_server_clear(s);
    free(s);
}

/* ── Load ───────────────────────────────────────────────────────── */

static void parse_env_object(mcp_server_t *s, json_value_t *env_obj) {
    if (!env_obj || !json_is_object(env_obj)) return;
    json_iter_t it = json_iter_object(env_obj);
    const char *key;
    json_value_t *val;
    while (json_iter_next(&it, &key, &val)) {
        if (!json_is_string(val)) continue;
        s->env_keys = realloc(s->env_keys, (s->env_count + 1) * sizeof(char *));
        s->env_values = realloc(s->env_values, (s->env_count + 1) * sizeof(char *));
        if (!s->env_keys || !s->env_values) continue;
        s->env_keys[s->env_count] = strdup(key);
        s->env_values[s->env_count] = strdup(json_string_value(val));
        s->env_count++;
    }
}

static void parse_args_array(mcp_server_t *s, json_value_t *args) {
    if (!args || !json_is_array(args)) return;
    int n = json_array_length(args);
    for (int i = 0; i < n; i++) {
        json_value_t *v = json_array_get(args, i);
        if (!json_is_string(v)) continue;
        s->args = realloc(s->args, (s->arg_count + 1) * sizeof(char *));
        if (!s->args) break;
        s->args[s->arg_count++] = strdup(json_string_value(v));
    }
}

mcp_config_t *mcp_config_load(void) {
    mcp_config_t *cfg = calloc(1, sizeof(mcp_config_t));
    if (!cfg) return NULL;

    char *path = mcp_config_path();
    if (!path) return cfg;
    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return cfg;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
        fclose(fp);
        return cfg;
    }
    char *data = malloc(sz + 1);
    size_t nread = fread(data, 1, sz, fp);
    data[nread] = '\0';
    fclose(fp);

    json_value_t *root = json_parse(data);
    free(data);
    if (!root) return cfg;

    json_value_t *servers = json_object_get(root, "servers");
    if (servers && json_is_object(servers)) {
        json_iter_t it = json_iter_object(servers);
        const char *name;
        json_value_t *val;
        while (json_iter_next(&it, &name, &val)) {
            if (!json_is_object(val)) continue;
            mcp_server_t *s = mcp_server_new(name);
            if (!s) continue;

            json_value_t *cmd = json_object_get(val, "command");
            if (cmd && json_is_string(cmd))
                s->command = strdup(json_string_value(cmd));
            json_value_t *cwd = json_object_get(val, "cwd");
            if (cwd && json_is_string(cwd))
                s->cwd = strdup(json_string_value(cwd));
            parse_args_array(s, json_object_get(val, "args"));
            parse_env_object(s, json_object_get(val, "env"));

            if (cfg->count >= cfg->cap) {
                cfg->cap = cfg->cap ? cfg->cap * 2 : 8;
                mcp_server_t *grown = realloc(cfg->servers,
                                              cfg->cap * sizeof(mcp_server_t));
                if (!grown) {
                    mcp_server_free(s);
                    break;
                }
                cfg->servers = grown;
            }
            cfg->servers[cfg->count++] = *s;
            free(s);
        }
    }
    json_free(root);
    return cfg;
}

/* ── Save ───────────────────────────────────────────────────────── */

int mcp_config_save(const mcp_config_t *cfg) {
    if (!cfg) return -1;

    json_value_t *root = json_object();
    json_value_t *servers = json_object();
    for (int i = 0; i < cfg->count; i++) {
        const mcp_server_t *s = &cfg->servers[i];
        json_value_t *j = json_object();
        json_object_set(j, "command",
            s->command ? json_string(s->command) : json_string(""));
        if (s->arg_count > 0) {
            json_value_t *args = json_array();
            for (int a = 0; a < s->arg_count; a++)
                json_array_append(args, json_string(s->args[a]));
            json_object_set(j, "args", args);
        }
        if (s->env_count > 0) {
            json_value_t *env = json_object();
            for (int e = 0; e < s->env_count; e++)
                json_object_set(env, s->env_keys[e],
                                json_string(s->env_values[e]));
            json_object_set(j, "env", env);
        }
        if (s->cwd && s->cwd[0])
            json_object_set(j, "cwd", json_string(s->cwd));
        json_object_set(servers, s->name, j);
    }
    json_object_set(root, "servers", servers);

    char *json_str = json_stringify_pretty(root);
    json_free(root);
    if (!json_str) return -1;

    char *path = mcp_config_path();
    if (!path) { free(json_str); return -1; }
    FILE *fp = fopen(path, "w");
    free(path);
    if (!fp) { free(json_str); return -1; }
    fputs(json_str, fp);
    fclose(fp);
    free(json_str);
    return 0;
}

/* ── Lookup / add / remove ──────────────────────────────────────── */

mcp_server_t *mcp_config_find(mcp_config_t *cfg, const char *name) {
    if (!cfg || !name) return NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->servers[i].name, name) == 0)
            return &cfg->servers[i];
    }
    return NULL;
}

int mcp_config_add(mcp_config_t *cfg, const char *name,
                   const char *command, char **args, int arg_count,
                   char **env, int env_count, const char *cwd, char **err) {
    if (!cfg || !name || !name[0] || !command || !command[0]) {
        if (err) *err = strdup("Name and --command are required");
        return -1;
    }

    mcp_server_t *s = mcp_config_find(cfg, name);
    if (!s) {
        if (cfg->count >= cfg->cap) {
            cfg->cap = cfg->cap ? cfg->cap * 2 : 8;
            mcp_server_t *grown = realloc(cfg->servers,
                                          cfg->cap * sizeof(mcp_server_t));
            if (!grown) {
                if (err) *err = strdup("Out of memory");
                return -1;
            }
            cfg->servers = grown;
        }
        s = &cfg->servers[cfg->count];
        memset(s, 0, sizeof(*s));
        s->name = strdup(name);
        cfg->count++;
    } else {
        /* Update existing — clear old fields, keep name */
        char *keep_name = strdup(s->name);
        mcp_server_clear(s);
        s->name = keep_name;
    }

    s->command = strdup(command);
    for (int i = 0; i < arg_count; i++) {
        s->args = realloc(s->args, (s->arg_count + 1) * sizeof(char *));
        if (!s->args) break;
        s->args[s->arg_count++] = strdup(args[i]);
    }
    for (int i = 0; i < env_count; i++) {
        /* Split KEY=VALUE */
        const char *eq = strchr(env[i], '=');
        if (!eq) continue;
        size_t klen = (size_t)(eq - env[i]);
        s->env_keys = realloc(s->env_keys, (s->env_count + 1) * sizeof(char *));
        s->env_values = realloc(s->env_values, (s->env_count + 1) * sizeof(char *));
        if (!s->env_keys || !s->env_values) break;
        s->env_keys[s->env_count] = strndup(env[i], klen);
        s->env_values[s->env_count] = strdup(eq + 1);
        s->env_count++;
    }
    s->cwd = cwd && cwd[0] ? strdup(cwd) : NULL;
    return 0;
}

int mcp_config_remove(mcp_config_t *cfg, const char *name) {
    if (!cfg || !name) return 0;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->servers[i].name, name) == 0) {
            mcp_server_clear(&cfg->servers[i]);
            cfg->servers[i] = cfg->servers[cfg->count - 1];
            cfg->count--;
            return 1;
        }
    }
    return 0;
}

void mcp_config_free(mcp_config_t *cfg) {
    if (!cfg) return;
    for (int i = 0; i < cfg->count; i++)
        mcp_server_clear(&cfg->servers[i]);
    free(cfg->servers);
    free(cfg);
}
