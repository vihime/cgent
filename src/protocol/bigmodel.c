/*
 * bigmodel.c — ZhipuAI BigModel (GLM) API provider
 *
 * OpenAI-compatible chat completions format.
 * API: POST https://open.bigmodel.cn/api/paas/v4/chat/completions
 */
#include "protocol.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Reuse OpenAI format — identical API structure */
extern api_provider_t provider_openai;

api_provider_t provider_bigmodel = {
    .type              = PROVIDER_BIGMODEL,
    .name              = "bigmodel",
    .default_base_url  = "https://open.bigmodel.cn/api/paas/v4/chat/completions",
    .default_model     = "glm-4.7",
    .auth_header       = "Authorization",
    .auth_prefix       = "Bearer ",
    .build_request     = NULL, /* Will be set from openai */
    .parse_response    = NULL,
    .parse_chunk       = NULL,
    .format_tool_results = NULL,
    .extract_tool_calls  = NULL,
};

/* Copy function pointers from openai provider on init */
void bigmodel_init(void) {
    provider_bigmodel.build_request     = provider_openai.build_request;
    provider_bigmodel.parse_response    = provider_openai.parse_response;
    provider_bigmodel.parse_chunk       = provider_openai.parse_chunk;
    provider_bigmodel.format_tool_results = provider_openai.format_tool_results;
    provider_bigmodel.extract_tool_calls  = provider_openai.extract_tool_calls;
}
