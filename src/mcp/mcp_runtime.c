/*
 * mcp_runtime.c — Bridge MCP server tools into the agent tool registry
 *
 * Each enabled MCP server is started as a session, its tools are
 * discovered via tools/list, and each tool is registered in the global
 * tool registry as "<server>__<tool>". When the model invokes one, the
 * handler forwards the call via tools/call and returns the result.
 */
#include "mcp.h"
#include "json.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MCP_TOOLS 512

/* Global map: prefixed registry name -> session + original tool name.
 * Mirrors the existing global tool registry design. */
typedef struct {
    char *tool_name;
    mcp_session_t *session;
    char *mcp_tool_name;
} mcp_registry_entry_t;

static mcp_registry_entry_t g_mcp_tools[MAX_MCP_TOOLS];
static int g_mcp_tool_count = 0;

static char *mcp_tool_handler(const char *name, const char *args_json,
                              char **error) {
    for (int i = 0; i < g_mcp_tool_count; i++) {
        if (strcmp(g_mcp_tools[i].tool_name, name) == 0) {
            return mcp_session_call_tool(g_mcp_tools[i].session,
                                         g_mcp_tools[i].mcp_tool_name,
                                         args_json, 60000, error);
        }
    }
    if (error) *error = strdup("MCP tool not found");
    return NULL;
}

int mcp_bridges_start(mcp_config_t *cfg, const char **names, int name_count,
                      bool all, mcp_bridge_t **bridges, int *out_count) {
    *bridges = NULL;
    *out_count = 0;
    if (!cfg) return 0;

    mcp_bridge_t *arr = NULL;
    int started = 0;

    for (int i = 0; i < cfg->count; i++) {
        mcp_server_t *srv = &cfg->servers[i];
        bool enabled = all;
        if (!enabled) {
            for (int j = 0; j < name_count; j++) {
                if (strcmp(srv->name, names[j]) == 0) {
                    enabled = true;
                    break;
                }
            }
        }
        if (!enabled) continue;

        char *err = NULL;
        mcp_session_t *s = mcp_session_start(srv, 15000, &err);
        if (!s) {
            fprintf(stderr, "[mcp] warning: failed to start server '%s': %s\n",
                    srv->name, err ? err : "unknown error");
            free(err);
            continue;
        }

        mcp_tool_info_t *tools = NULL;
        int n = 0;
        if (mcp_session_list_tools(s, &tools, &n, &err) != 0 || n == 0) {
            fprintf(stderr, "[mcp] warning: server '%s' exposed no tools%s%s\n",
                    srv->name, err ? ": " : "", err ? err : "");
            free(err);
            mcp_session_stop(s);
            mcp_tool_info_free(tools, n);
            continue;
        }
        free(err);

        mcp_bridge_t *grown = realloc(arr, (started + 1) * sizeof(mcp_bridge_t));
        if (!grown) {
            fprintf(stderr, "[mcp] warning: out of memory, skipping '%s'\n",
                    srv->name);
            mcp_session_stop(s);
            mcp_tool_info_free(tools, n);
            break;
        }
        arr = grown;
        mcp_bridge_t *b = &arr[started];
        memset(b, 0, sizeof(*b));
        b->server_name = strdup(srv->name);
        b->session = s;
        b->tools = calloc(n, sizeof(tool_t));
        if (!b->tools) {
            mcp_session_stop(s);
            mcp_tool_info_free(tools, n);
            free(b->server_name);
            break;
        }
        b->tool_count = 0;

        for (int t = 0; t < n; t++) {
            char full[1024];
            snprintf(full, sizeof(full), "%s__%s", srv->name, tools[t].name);
            if (tool_registry_find(full)) {
                fprintf(stderr, "[mcp] skipping duplicate tool '%s'\n", full);
                continue;
            }

            tool_t *tool = &b->tools[b->tool_count];
            memset(tool, 0, sizeof(*tool));
            tool->name = strdup(full);
            size_t dlen = strlen(tools[t].description) + strlen(srv->name) + 64;
            tool->description = malloc(dlen);
            if (tool->description)
                snprintf(tool->description, dlen, "%s\n(MCP server: %s)",
                         tools[t].description, srv->name);
            tool->parameters_schema = strdup(
                tools[t].input_schema && tools[t].input_schema[0]
                    ? tools[t].input_schema : "{\"type\":\"object\"}");
            tool->handler = mcp_tool_handler;
            tool->userdata = NULL;
            /* MCP sessions share one stdio pipe — not concurrency-safe */
            tool->thread_safe = false;

            if (g_mcp_tool_count < MAX_MCP_TOOLS) {
                g_mcp_tools[g_mcp_tool_count].tool_name = strdup(full);
                g_mcp_tools[g_mcp_tool_count].session = s;
                g_mcp_tools[g_mcp_tool_count].mcp_tool_name =
                    strdup(tools[t].name);
                g_mcp_tool_count++;
            }
            tool_registry_add(tool);
            b->tool_count++;
        }

        mcp_tool_info_free(tools, n);
        started++;
    }

    *bridges = arr;
    *out_count = started;
    return 0;
}

void mcp_bridges_stop(mcp_bridge_t *bridges, int count) {
    if (!bridges) return;
    for (int i = 0; i < count; i++) {
        mcp_bridge_t *b = &bridges[i];
        for (int t = 0; t < b->tool_count; t++) {
            tool_registry_remove(b->tools[t].name);
            for (int g = 0; g < g_mcp_tool_count; g++) {
                if (g_mcp_tools[g].tool_name &&
                    strcmp(g_mcp_tools[g].tool_name, b->tools[t].name) == 0) {
                    free(g_mcp_tools[g].tool_name);
                    free(g_mcp_tools[g].mcp_tool_name);
                    g_mcp_tools[g] = g_mcp_tools[--g_mcp_tool_count];
                    break;
                }
            }
            /* Array element — free fields, not the struct itself */
            free(b->tools[t].name);
            free(b->tools[t].description);
            free(b->tools[t].parameters_schema);
        }
        free(b->tools);
        mcp_session_stop(b->session);
        free(b->server_name);
    }
    free(bridges);
}
