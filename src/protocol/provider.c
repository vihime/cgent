/*
 * provider.c — API provider registry
 */
#include "protocol.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static api_provider_t *providers[PROVIDER_COUNT] = {0};

/* Forward declarations */
extern api_provider_t provider_deepseek;
extern api_provider_t provider_openai;
extern api_provider_t provider_anthropic;

void provider_init(void) {
    providers[PROVIDER_DEEPSEEK]  = &provider_deepseek;
    providers[PROVIDER_OPENAI]    = &provider_openai;
    providers[PROVIDER_ANTHROPIC] = &provider_anthropic;
}

api_provider_t *provider_get(provider_type_t type) {
    if (type < 0 || type >= PROVIDER_COUNT) return NULL;
    return providers[type];
}

api_provider_t *provider_get_by_name(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "deepseek") == 0)  return providers[PROVIDER_DEEPSEEK];
    if (strcmp(name, "openai") == 0)    return providers[PROVIDER_OPENAI];
    if (strcmp(name, "anthropic") == 0) return providers[PROVIDER_ANTHROPIC];
    return NULL;
}
