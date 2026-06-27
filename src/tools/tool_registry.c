/*
 * tool_registry.c — Tool registration and lookup
 */
#include "tools.h"

#include <stdlib.h>
#include <string.h>

/* Static registry for now (will be per-agent later) */
#define MAX_REGISTRY 1024
static tool_t *g_registry[MAX_REGISTRY];
static int g_registry_count = 0;

int tool_registry_add(tool_t *tool) {
    if (!tool || g_registry_count >= MAX_REGISTRY) return -1;
    g_registry[g_registry_count++] = tool;
    return 0;
}

void tool_registry_remove(const char *name) {
    for (int i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i]->name, name) == 0) {
            g_registry[i] = g_registry[--g_registry_count];
            return;
        }
    }
}

tool_t *tool_registry_find(const char *name) {
    for (int i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i]->name, name) == 0)
            return g_registry[i];
    }
    return NULL;
}

tool_t *tool_registry_all(int *count) {
    if (count) *count = g_registry_count;
    return g_registry_count > 0 ? g_registry[0] : NULL;
}

int registry_count(void) {
    return g_registry_count;
}

const tool_t *registry_get(int index) {
    if (index < 0 || index >= g_registry_count) return NULL;
    return g_registry[index];
}

void tool_registry_clear(void) {
    g_registry_count = 0;
}
