/*
 * tools.h — Tool registry and execution
 */
#ifndef TOOLS_H
#define TOOLS_H

#include "core.h"

/* ── Tool registry ──────────────────────────────────────────────── */

/* Register a tool and its handler */
int  tool_registry_add(tool_t *tool);
void tool_registry_remove(const char *name);

/* Find a tool by name */
tool_t *tool_registry_find(const char *name);

/* Get all registered tools */
tool_t *tool_registry_all(int *count);

/* Get registry size */
int registry_count(void);

/* Get tool at index */
const tool_t *registry_get(int index);

/* Clear the registry */
void tool_registry_clear(void);

/* ── Tool execution ─────────────────────────────────────────────── */

/* Execute a tool by name with JSON arguments.
 * Returns the result string (caller must free).
 * On error, *error is set. */
char *tool_execute(const char *name, const char *args_json,
                   int timeout_ms, char **error);

/* ── Built-in tool registration ─────────────────────────────────── */

/* Register all built-in tools (read_file, write_file, bash, think).
 * These are always available to every agent. */
void builtin_tools_register(void);

#endif /* TOOLS_H */
