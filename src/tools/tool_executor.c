/*
 * tool_executor.c — Tool execution with timeout
 */
#include "tools.h"

#include <stdlib.h>
#include <string.h>

char *tool_execute(const char *name, const char *args_json,
                   int timeout_ms, char **error) {
    (void)timeout_ms; /* TODO: implement timeout via alarm/sigaction */

    tool_t *tool = tool_registry_find(name);
    if (!tool) {
        if (error) *error = strdup("Tool not found");
        return NULL;
    }

    if (!tool->handler) {
        if (error) *error = strdup("Tool has no handler");
        return NULL;
    }

    char *result = tool->handler(name, args_json, error);
    return result;
}
