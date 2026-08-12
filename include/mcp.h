/*
 * mcp.h — MCP (Model Context Protocol) server management
 *
 * Servers are configured in ~/.cgent/mcp.json:
 * {
 *   "servers": {
 *     "filesystem": {
 *       "command": "npx",
 *       "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
 *       "env": {"FOO": "bar"},
 *       "cwd": "/home/user"
 *     }
 *   }
 * }
 */
#ifndef MCP_H
#define MCP_H

#include <stdbool.h>
#include "core.h"

/* ── MCP server configuration ───────────────────────────────────── */

typedef struct {
    char *name;
    char *command;          /* executable to launch (via PATH lookup) */
    char **args;            /* command-line arguments */
    int arg_count;
    char **env_keys;        /* extra environment variables */
    char **env_values;
    int env_count;
    char *cwd;              /* optional working directory */
} mcp_server_t;

typedef struct {
    mcp_server_t *servers;
    int count;
    int cap;
} mcp_config_t;

/* Load ~/.cgent/mcp.json; returns an empty config if the file is
 * missing or unreadable. Caller must free with mcp_config_free. */
mcp_config_t *mcp_config_load(void);

/* Free a config and all contained servers. */
void mcp_config_free(mcp_config_t *cfg);

/* Write the config to ~/.cgent/mcp.json. Returns 0 on success, -1 on
 * failure. */
int mcp_config_save(const mcp_config_t *cfg);

/* Find a server by name, or NULL. */
mcp_server_t *mcp_config_find(mcp_config_t *cfg, const char *name);

/* Add a new server or update an existing one.
 * env is an array of "KEY=VALUE" strings. On success returns 0; on
 * failure returns -1 with *err set (caller must free *err). */
int mcp_config_add(mcp_config_t *cfg, const char *name,
                   const char *command, char **args, int arg_count,
                   char **env, int env_count, const char *cwd, char **err);

/* Remove a server by name. Returns 1 if removed, 0 if not found. */
int mcp_config_remove(mcp_config_t *cfg, const char *name);

/* ── MCP server test (stdio JSON-RPC handshake) ─────────────────── */

/* Spawn the server process, perform the MCP initialize handshake, then
 * list available tools. Returns a malloc'd JSON string:
 *   {"success":true,"name":...,"version":...,"protocol_version":...,
 *    "capabilities":[...],"tools_count":N,"tools":[...]}
 * or {"success":false,"error":...} on failure. Caller must free. */
char *mcp_server_test(const mcp_server_t *srv, int timeout_ms);

/* ── MCP session (running stdio server) ────────────────────────── */

/* Opaque handle to a running MCP server process. */
typedef struct mcp_session mcp_session_t;

/* A tool exposed by an MCP server. */
typedef struct {
    char *name;
    char *description;
    char *input_schema;         /* JSON Schema as a string */
} mcp_tool_info_t;

/* Spawn the server and perform the initialize handshake.
 * On failure returns NULL with *err set. */
mcp_session_t *mcp_session_start(const mcp_server_t *srv, int timeout_ms,
                                 char **err);

/* Discover tools via tools/list. On success returns 0 and allocates
 * *tools (caller frees each entry + array); on failure returns -1. */
int mcp_session_list_tools(mcp_session_t *s, mcp_tool_info_t **tools,
                           int *count, char **err);

/* Invoke a tool via tools/call with JSON arguments. Returns a malloc'd
 * JSON result string (content + is_error + optional structured), or
 * NULL with *err set on failure. */
char *mcp_session_call_tool(mcp_session_t *s, const char *tool_name,
                            const char *args_json, int timeout_ms,
                            char **err);

/* Free discovered tool info (each entry + array). */
void mcp_tool_info_free(mcp_tool_info_t *tools, int count);

/* Terminate the server process and free the session. */
void mcp_session_stop(mcp_session_t *s);

/* ── Agent bridge (register MCP tools as agent tools) ───────────── */

typedef struct {
    char *server_name;
    mcp_session_t *session;
    tool_t *tools;              /* tool_t entries (registered globally) */
    int tool_count;
} mcp_bridge_t;

/* Start the named servers (or all configured when all=true), discover
 * their tools, and register them in the global tool registry under
 * "<server>__<tool>" names. On failure of an individual server, a
 * warning is printed to stderr and the remaining servers continue.
 * Returns the number of successfully started bridges in *out_count.
 * Caller must free *bridges with mcp_bridges_stop. */
int mcp_bridges_start(mcp_config_t *cfg, const char **names, int name_count,
                      bool all, mcp_bridge_t **bridges, int *out_count);

/* Unregister MCP tools, kill sessions, and free everything. */
void mcp_bridges_stop(mcp_bridge_t *bridges, int count);

/* ── CLI entry point: cgent mcp <subcommand> ... ────────────────── */

int mcp_main(int argc, char **argv);

#endif /* MCP_H */
