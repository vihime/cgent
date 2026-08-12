/*
 * agent.c — Agent lifecycle, conversation loop, tool-use handling
 *
 * The central module that orchestrates:
 *   1. Building the API request from conversation state
 *   2. Sending the request via HTTP/TLS
 *   3. Parsing the response (non-streaming or SSE streaming)
 *   4. Executing tool calls
 *   5. Looping until the assistant produces a final text response
 */
#include "cgent.h"
#include "core.h"
#include "protocol.h"
#include "network.h"
#include "tools.h"

#include <pthread.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Retry / usage helpers ──────────────────────────────────────── */

static bool http_status_transient(int code) {
    return code == 429 || (code >= 500 && code <= 504);
}

static void agent_sleep_ms(int ms) {
    if (ms > 0) usleep((useconds_t)ms * 1000);
}

/* Exponential backoff (500ms, 1s, 2s, ...) with ±20% jitter. */
static int retry_delay_ms(int attempt) {
    int base = 500 * (1 << attempt);
    int jitter = base / 5;
    return base - jitter + (rand() % (2 * jitter + 1));
}

/* Account one completed API request (usage may be 0 when unknown). */
static void agent_record_usage(agent_t *agent, const message_t *msg) {
    if (!agent || !msg) return;
    agent->request_count++;
    if (msg->prompt_tokens > 0) agent->prompt_tokens += msg->prompt_tokens;
    if (msg->completion_tokens > 0)
        agent->completion_tokens += msg->completion_tokens;
}

/* ── Context management ─────────────────────────────────────────── */

long long agent_estimate_tokens(const char *text) {
    if (!text) return 0;
    long long ascii = 0, cjk = 0, other = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) { ascii++; p++; continue; }
        uint32_t cp = 0;
        int len = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { other++; p++; continue; }
        for (int i = 1; i < len && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        /* CJK ranges: ~1 token per character */
        if ((cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0x3000 && cp <= 0x303F) ||
            (cp >= 0xFF00 && cp <= 0xFFEF)) {
            cjk++;
        } else {
            other++;
        }
        p += len;
    }
    return ascii / 4 + cjk + other / 2;
}

void agent_context_stats(const agent_t *agent, agent_context_stats_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    if (!agent) return;

    st->system_tokens = agent_estimate_tokens(agent->system_prompt);
    for (int i = 0; i < agent->n_tools; i++) {
        st->tool_tokens += agent_estimate_tokens(agent->tools[i].name)
                         + agent_estimate_tokens(agent->tools[i].description)
                         + agent_estimate_tokens(agent->tools[i].parameters_schema);
    }
    for (int i = 0; i < agent->n_messages; i++) {
        const message_t *m = &agent->messages[i];
        st->message_tokens += agent_estimate_tokens(m->content)
                            + agent_estimate_tokens(m->reasoning_content)
                            + agent_estimate_tokens(m->name);
        for (int j = 0; j < m->n_tool_calls; j++) {
            st->message_tokens += agent_estimate_tokens(m->tool_calls[j].name)
                                + agent_estimate_tokens(m->tool_calls[j].arguments);
        }
        for (int j = 0; j < m->n_tool_results; j++) {
            st->message_tokens += agent_estimate_tokens(m->tool_results[j].content);
        }
    }
    st->total_tokens = st->system_tokens + st->tool_tokens + st->message_tokens;
}

/* Build a plain-text transcript of the conversation (for compaction). */
static char *agent_build_transcript(const agent_t *agent) {
    if (!agent || agent->n_messages == 0) return NULL;
    size_t buf_sz = 64 * 1024;
    size_t buf_len = 0;
    char *transcript = malloc(buf_sz);
    if (!transcript) return NULL;
    transcript[0] = '\0';

    for (int i = 0; i < agent->n_messages; i++) {
        const message_t *m = &agent->messages[i];
        const char *role_str = "?";
        switch (m->role) {
        case MSG_ROLE_USER:      role_str = "USER"; break;
        case MSG_ROLE_ASSISTANT: role_str = "ASSISTANT"; break;
        case MSG_ROLE_TOOL:      role_str = "TOOL"; break;
        default: break;
        }

        char msg_buf[8192];
        if (m->content && m->content[0])
            snprintf(msg_buf, sizeof(msg_buf), "[%s]: %s\n", role_str, m->content);
        else
            snprintf(msg_buf, sizeof(msg_buf), "[%s]: (no text)\n", role_str);

        char tc_buf[4096];
        tc_buf[0] = '\0';
        if (m->n_tool_calls > 0) {
            int off = 0;
            off += snprintf(tc_buf + off, sizeof(tc_buf) - off, "  Tool calls:\n");
            for (int j = 0; j < m->n_tool_calls && off < (int)sizeof(tc_buf) - 1; j++) {
                const char *args = m->tool_calls[j].arguments
                                   ? m->tool_calls[j].arguments : "{}";
                size_t alen = strlen(args);
                int show = (int)(alen > 500 ? 500 : alen);
                off += snprintf(tc_buf + off, sizeof(tc_buf) - off,
                                "    - %s: %.*s%s\n", m->tool_calls[j].name,
                                show, args, alen > 500 ? "...(truncated)" : "");
            }
        }

        char tr_buf[4096];
        tr_buf[0] = '\0';
        if (m->n_tool_results > 0) {
            int off = 0;
            off += snprintf(tr_buf + off, sizeof(tr_buf) - off, "  Tool results:\n");
            for (int j = 0; j < m->n_tool_results && off < (int)sizeof(tr_buf) - 1; j++) {
                const char *content = m->tool_results[j].content
                                      ? m->tool_results[j].content : "";
                size_t clen = strlen(content);
                int show = (int)(clen > 2000 ? 2000 : clen);
                off += snprintf(tr_buf + off, sizeof(tr_buf) - off,
                                "    [%s]: %.*s%s\n",
                                m->tool_results[j].is_error ? "ERROR" : "OK",
                                show, content,
                                clen > 2000 ? "...(truncated)" : "");
            }
        }

        size_t needed = strlen(msg_buf) + strlen(tc_buf) + strlen(tr_buf) + 2;
        while (buf_len + needed + 1 > buf_sz) {
            buf_sz *= 2;
            char *grown = realloc(transcript, buf_sz);
            if (!grown) { free(transcript); return NULL; }
            transcript = grown;
        }
        strcpy(transcript + buf_len, msg_buf);
        buf_len += strlen(msg_buf);
        if (tc_buf[0]) {
            strcpy(transcript + buf_len, tc_buf);
            buf_len += strlen(tc_buf);
        }
        if (tr_buf[0]) {
            strcpy(transcript + buf_len, tr_buf);
            buf_len += strlen(tr_buf);
        }
        transcript[buf_len++] = '\n';
        transcript[buf_len] = '\0';
    }
    return transcript;
}

int agent_compact(agent_t *agent) {
    if (!agent || agent->n_messages == 0) return -1;

    char *transcript = agent_build_transcript(agent);
    if (!transcript) return -1;

    static const char *compact_instruction =
        "You are compressing the conversation history of an AI coding "
        "assistant session. Your task is to produce a structured summary "
        "that preserves ONLY the information needed to continue work "
        "effectively:\n\n"
        "REQUIRED - must preserve:\n"
        "- The user's final/current task goal\n"
        "- Important technical decisions made (with rationale)\n"
        "- Files modified and key code changes (what was changed and why)\n"
        "- Project architecture information (language, framework, structure)\n\n"
        "DISCARD:\n"
        "- Trial-and-error attempts and failed approaches\n"
        "- Reverted/modified changes that no longer apply\n"
        "- Verbose tool outputs, logs, debug output\n"
        "- Repetitive or redundant exchanges\n"
        "- Irrelevant tangents\n\n"
        "Format the summary as a compact but complete technical document. "
        "Keep it concise. Write in prose, not bullet points.\n\n"
        "Here is the conversation to compress:\n\n"
        "---BEGIN CONVERSATION---\n";

    size_t prompt_sz = strlen(compact_instruction) + strlen(transcript) + 64;
    char *prompt = malloc(prompt_sz);
    if (!prompt) { free(transcript); return -1; }
    snprintf(prompt, prompt_sz, "%s%s\n---END CONVERSATION---\n",
             compact_instruction, transcript);
    free(transcript);

    int saved_count = agent->n_messages;

    /* Clear conversation — agent_chat will populate fresh */
    for (int i = 0; i < agent->n_messages; i++)
        message_clear(&agent->messages[i]);
    agent->n_messages = 0;

    /* Send compaction request; suppress auto-compact re-entry */
    agent->compacting = true;
    message_t *resp = agent_chat(agent, prompt);
    agent->compacting = false;
    free(prompt);

    char *compressed = NULL;
    if (resp && resp->content && resp->content[0])
        compressed = strdup(resp->content);
    message_free(resp);

    /* Clear again — don't keep the compaction request/response */
    for (int i = 0; i < agent->n_messages; i++)
        message_clear(&agent->messages[i]);
    agent->n_messages = 0;

    if (!compressed) return -1;

    size_t ctx_sz = strlen(compressed) + 128;
    char *ctx_msg = malloc(ctx_sz);
    if (!ctx_msg) { free(compressed); return -1; }
    snprintf(ctx_msg, ctx_sz,
             "[Compressed conversation summary — %d messages condensed]\n\n%s",
             saved_count, compressed);
    free(compressed);

    message_t ctx_message = {
        .role = MSG_ROLE_USER,
        .content = ctx_msg,
        .n_tool_calls = 0,
        .n_tool_results = 0,
    };
    agent_add_message(agent, &ctx_message);
    free(ctx_msg);
    return saved_count;
}

/* Drop the oldest messages (sliding window) to shrink the context. */
static void agent_trim_old_messages(agent_t *agent) {
    if (!agent || agent->n_messages < 4) return;
    int keep = agent->n_messages / 2;
    if (keep < 4) keep = 4;
    int drop = agent->n_messages - keep;
    for (int i = 0; i < drop; i++)
        message_clear(&agent->messages[i]);
    memmove(&agent->messages[0], &agent->messages[drop],
            keep * sizeof(message_t));
    agent->n_messages = keep;
}

/* Automatic context management: compact or trim before a request when
 * the estimated conversation size approaches the context window. */
static void agent_maybe_manage_context(agent_t *agent) {
    if (!agent || agent->compacting) return;
    if (!agent->provider.auto_compact) return;
    if (agent->provider.context_length <= 0) return;

    long long limit = (long long)(agent->provider.context_length
                                  * agent->provider.compact_ratio);
    agent_context_stats_t st;
    agent_context_stats(agent, &st);
    if (st.total_tokens <= limit) return;

    fprintf(stderr,
            "[context] estimated %lld/%lld tokens, compacting conversation...\n",
            st.total_tokens, (long long)agent->provider.context_length);
    if (agent_compact(agent) > 0) {
        agent_context_stats(agent, &st);
        if (st.total_tokens <= limit) return;
        fprintf(stderr,
                "[context] still %lld tokens after compaction, trimming history\n",
                st.total_tokens);
    } else {
        fprintf(stderr, "[context] compaction failed, trimming history\n");
    }
    agent_trim_old_messages(agent);
}

/* ── Tool execution (parallel where safe) ───────────────────────── */

typedef struct {
    pthread_t thread;
    bool thread_started;
    const tool_call_t *tc;
    char *result;               /* tool output or NULL */
    char *error;                /* error string or NULL */
} tool_job_t;

static void *tool_job_run(void *arg) {
    tool_job_t *job = (tool_job_t *)arg;
    job->result = tool_execute(job->tc->name, job->tc->arguments,
                               30000, &job->error);
    return NULL;
}

/* Execute all tool calls of an assistant message and append the tool
 * result messages to the conversation (in tool-call order). Tools
 * marked thread_safe run in parallel; others run sequentially. */
static int agent_execute_tools(agent_t *agent, message_t *assistant_msg) {
    if (!agent || !assistant_msg) return -1;
    int n = assistant_msg->n_tool_calls;
    if (n <= 0) return 0;

    bool parallel = agent->provider.parallel_tools;
    tool_job_t *jobs = calloc(n, sizeof(tool_job_t));
    if (!jobs) return -1;

    for (int i = 0; i < n; i++) {
        jobs[i].tc = &assistant_msg->tool_calls[i];
        if (agent->verbose) {
            fprintf(stderr, "[agent] Executing tool: %s(%s)\n",
                    jobs[i].tc->name, jobs[i].tc->arguments);
        }
    }

    /* Launch parallel jobs for thread-safe tools (cap at 32 threads) */
    int launched = 0;
    if (parallel) {
        for (int i = 0; i < n && launched < 32; i++) {
            tool_t *tool = tool_registry_find(assistant_msg->tool_calls[i].name);
            if (tool && !tool->thread_safe) continue;
            if (pthread_create(&jobs[i].thread, NULL, tool_job_run,
                               &jobs[i]) == 0) {
                jobs[i].thread_started = true;
                launched++;
            } else {
                /* Thread creation failed — run inline */
                jobs[i].result = tool_execute(jobs[i].tc->name,
                                              jobs[i].tc->arguments,
                                              30000, &jobs[i].error);
            }
        }
    }

    /* Run non-thread-safe tools sequentially in the calling thread */
    for (int i = 0; i < n; i++) {
        if (jobs[i].thread_started) continue;
        tool_t *tool = tool_registry_find(assistant_msg->tool_calls[i].name);
        bool can_parallel = tool && tool->thread_safe;
        if (parallel && can_parallel) continue; /* already handled */
        jobs[i].result = tool_execute(jobs[i].tc->name, jobs[i].tc->arguments,
                                      30000, &jobs[i].error);
    }

    /* Collect parallel results */
    for (int i = 0; i < n; i++) {
        if (jobs[i].thread_started)
            pthread_join(jobs[i].thread, NULL);
    }

    /* Append tool result messages in tool-call order */
    for (int i = 0; i < n; i++) {
        if (agent->verbose) {
            if (jobs[i].result) {
                size_t rlen = strlen(jobs[i].result);
                fprintf(stderr, "[agent] Tool result (%s): %.*s%s\n",
                        jobs[i].tc->name,
                        (int)(rlen < 400 ? rlen : 400), jobs[i].result,
                        rlen > 400 ? "..." : "");
            } else {
                fprintf(stderr, "[agent] Tool error (%s): %s\n",
                        jobs[i].tc->name,
                        jobs[i].error ? jobs[i].error : "unknown");
            }
        }

        message_t tool_msg = {
            .role = MSG_ROLE_TOOL,
            .content = NULL,
            .n_tool_calls = 0,
            .n_tool_results = 1,
            .tool_results = calloc(1, sizeof(tool_result_t)),
        };
        tool_msg.tool_results[0].tool_call_id = strdup(jobs[i].tc->id);
        tool_msg.tool_results[0].content = jobs[i].result
            ? jobs[i].result
            : strdup(jobs[i].error ? jobs[i].error : "");
        tool_msg.tool_results[0].is_error = (jobs[i].result == NULL);
        free(jobs[i].error);
        agent_add_message(agent, &tool_msg);
    }

    free(jobs);
    return 0;
}

/* ── Agent lifecycle ────────────────────────────────────────────── */

agent_t *agent_create(provider_config_t *config, struct api_provider *api) {
    agent_t *agent = calloc(1, sizeof(agent_t));
    if (!agent) return NULL;

    if (config) {
        agent->provider.api_key          = config->api_key ? strdup(config->api_key) : NULL;
        agent->provider.base_url         = config->base_url ? strdup(config->base_url) : NULL;
        agent->provider.model            = config->model ? strdup(config->model) : NULL;
        agent->provider.temperature      = config->temperature;
        agent->provider.max_tokens       = config->max_tokens;
        agent->provider.stream           = config->stream;
        agent->provider.thinking_enabled   = config->thinking_enabled;
        agent->provider.thinking_configured = config->thinking_configured;
        agent->provider.max_retries      = config->max_retries;
        agent->provider.context_length   = config->context_length;
        agent->provider.auto_compact     = config->auto_compact;
        agent->provider.compact_ratio    = config->compact_ratio;
        agent->provider.parallel_tools   = config->parallel_tools;
        agent->provider.reasoning_effort   = config->reasoning_effort
                                           ? strdup(config->reasoning_effort) : NULL;
    }
    agent->api = api;

    agent->cap_messages = 64;
    agent->messages = calloc(agent->cap_messages, sizeof(message_t));
    agent->n_messages = 0;

    agent->cap_tools = 32;
    agent->tools = calloc(agent->cap_tools, sizeof(tool_t));
    agent->n_tools = 0;

    return agent;
}

void agent_free(agent_t *agent) {
    if (!agent) return;
    free(agent->provider.api_key);
    free(agent->provider.base_url);
    free(agent->provider.model);
    free(agent->provider.reasoning_effort);
    free(agent->system_prompt);
    for (int i = 0; i < agent->n_messages; i++)
        message_clear(&agent->messages[i]);
    free(agent->messages);
    /* Tools are owned by the global registry — don't free them here */
    free(agent->tools);
    free(agent);
}

void agent_set_system_prompt(agent_t *agent, const char *prompt) {
    if (!agent) return;
    free(agent->system_prompt);
    agent->system_prompt = prompt ? strdup(prompt) : NULL;
}

int agent_add_tool(agent_t *agent, const tool_t *tool) {
    if (!agent || !tool) return -1;
    if (agent->n_tools >= agent->cap_tools) {
        agent->cap_tools *= 2;
        agent->tools = realloc(agent->tools, agent->cap_tools * sizeof(tool_t));
    }
    agent->tools[agent->n_tools] = *tool;
    agent->n_tools++;
    return 0;
}

int agent_add_message(agent_t *agent, const message_t *msg) {
    if (!agent || !msg) return -1;
    if (agent->n_messages >= agent->cap_messages) {
        agent->cap_messages *= 2;
        agent->messages = realloc(agent->messages,
                                  agent->cap_messages * sizeof(message_t));
    }
    /* Deep copy to avoid double-free issues */
    message_t *copy = message_copy(msg);
    if (!copy) return -1;
    agent->messages[agent->n_messages++] = *copy;
    free(copy); /* Free the wrapper struct, not the contents (now owned by agent) */
    return 0;
}

/* ── HTTP request helper ────────────────────────────────────────── */

/* Build the API endpoint URL from provider base URL */
static char *agent_endpoint_url(agent_t *agent) {
    const char *base = agent->provider.base_url;
    if (!base) base = agent->api->default_base_url;

    /* If base_url already contains the full endpoint, use it directly */
    if (strstr(base, "/chat/completions") || strstr(base, "/messages")) {
        return strdup(base);
    }

    /* DeepSeek and OpenAI use chat completions endpoint */
    if (agent->api->type == PROVIDER_ANTHROPIC) {
        size_t len = strlen(base) + 16;
        char *url = malloc(len);
        snprintf(url, len, "%s/v1/messages", base);
        return url;
    } else {
        size_t len = strlen(base) + 32;
        char *url = malloc(len);
        snprintf(url, len, "%s/v1/chat/completions", base);
        return url;
    }
}

/* Build authorization header value */
static char *agent_auth_header(agent_t *agent) {
    const char *prefix = agent->api->auth_prefix ? agent->api->auth_prefix : "Bearer ";
    size_t len = strlen(prefix) + strlen(agent->provider.api_key) + 1;
    char *val = malloc(len);
    snprintf(val, len, "%s%s", prefix, agent->provider.api_key);
    return val;
}

/* ── SSE-aware response parsing ─────────────────────────────────── */

/* Parse an HTTP response body that could be either:
 *   1. Plain JSON: {"choices":[...]} (stream=false)
 *   2. SSE stream:  data: {...}\n\ndata: {...}\n\n... (stream=true)
 *
 * For SSE, accumulates all deltas via the provider's parse_chunk.
 * Calls on_token for each text delta (for streaming output).
 * Returns the full accumulated message, or NULL on failure. */
static message_t *agent_parse_response_body(api_provider_t *api,
                                             const char *body,
                                             bool verbose,
                                             void (*on_token)(const char *token, void *ctx),
                                             void *token_ctx) {
    if (!body || !body[0]) return NULL;

    /* Detect SSE format: starts with "data: " */
    if (strncmp(body, "data: ", 6) == 0) {
        if (verbose) {
            fprintf(stderr, "[agent] Detected SSE streaming response\n");
        }

        /* Accumulator message */
        message_t *accum = message_create(MSG_ROLE_ASSISTANT, NULL);
        if (!accum) return NULL;

        /* Accumulated text */
        char *text_buf = NULL;
        size_t text_len = 0;

        /* Tool call merger: SSE spreads tool call fields across chunks.
         * Track up to 16 in-progress tool calls by index, merging fields. */
        #define MAX_PENDING_TC 16
        typedef struct {
            int index;
            char *id;
            char *name;
            char *arguments;
        } pending_tc_t;
        pending_tc_t pending_tcs[MAX_PENDING_TC];
        int n_pending = 0;
        memset(pending_tcs, 0, sizeof(pending_tcs));

        /* Split by "\n\n" (SSE event boundary) */
        const char *p = body;
        while (*p) {
            /* Find next event boundary */
            const char *end = strstr(p, "\n\n");
            if (!end) end = p + strlen(p);

            /* Process lines within this event */
            const char *line_start = p;
            while (line_start < end) {
                const char *line_end = strchr(line_start, '\n');
                if (!line_end || line_end > end) line_end = end;

                size_t line_len = line_end - line_start;
                /* Ignore empty lines and comments */
                if (line_len > 0 && line_start[0] != ':') {
                    /* Check for "data: " prefix */
                    if (line_len > 6 && strncmp(line_start, "data: ", 6) == 0) {
                        const char *json_start = line_start + 6;
                        size_t json_len = line_len - 6;

                        /* Extract JSON string (not null-terminated within line) */
                        char *json_str = strndup(json_start, json_len);

                        /* Check for [DONE] marker */
                        if (strcmp(json_str, "[DONE]") == 0) {
                            free(json_str);
                            goto sse_done;
                        }

                        /* Parse this chunk via the provider */
                        message_t *delta = api->parse_chunk(json_str);

                        /* Also parse the raw JSON to extract tool_call index */
                        json_value_t *raw = json_parse(json_str);
                        int tc_index = -1;
                        if (raw) {
                            json_value_t *choices = json_object_get(raw, "choices");
                            if (choices && json_array_length(choices) > 0) {
                                json_value_t *c0 = json_array_get(choices, 0);
                                json_value_t *d = json_object_get(c0, "delta");
                                if (d) {
                                    json_value_t *tcs = json_object_get(d, "tool_calls");
                                    if (tcs && json_array_length(tcs) > 0) {
                                        json_value_t *tc0 = json_array_get(tcs, 0);
                                        json_value_t *idx = json_object_get(tc0, "index");
                                        if (idx) tc_index = (int)json_number_value(idx);
                                    }
                                }
                            }
                            json_free(raw);
                        }
                        free(json_str);

                        if (delta) {
                            /* Accumulate text content and emit via callback */
                            if (delta->content) {
                                size_t dlen = strlen(delta->content);
                                text_buf = realloc(text_buf, text_len + dlen + 1);
                                memcpy(text_buf + text_len, delta->content, dlen);
                                text_len += dlen;
                                text_buf[text_len] = '\0';
                                /* Emit this chunk immediately for streaming effect */
                                if (on_token) {
                                    on_token(delta->content, token_ctx);
                                    /* Small delay for visible streaming effect */
                                    usleep(15000); /* 15ms */
                                }
                            }
                            /* Accumulate tool calls — merge by index */
                            for (int i = 0; i < delta->n_tool_calls; i++) {
                                tool_call_t *tc = &delta->tool_calls[i];

                                /* Use the index from raw JSON, or fall back to loop counter */
                                int idx = (tc_index >= 0) ? tc_index : i;

                                /* Find or create pending tool call entry */
                                pending_tc_t *pt = NULL;
                                for (int j = 0; j < n_pending; j++) {
                                    if (pending_tcs[j].index == idx) {
                                        pt = &pending_tcs[j];
                                        break;
                                    }
                                }
                                if (!pt && n_pending < MAX_PENDING_TC) {
                                    pt = &pending_tcs[n_pending++];
                                    pt->index = idx;
                                }

                                if (pt) {
                                    /* Merge fields:
                                     * - id and name: overwrite (set once in first chunk)
                                     * - arguments: CONCATENATE (streamed in fragments) */
                                    if (tc->id && tc->id[0]) {
                                        free(pt->id);
                                        pt->id = strdup(tc->id);
                                    }
                                    if (tc->name && tc->name[0]) {
                                        free(pt->name);
                                        pt->name = strdup(tc->name);
                                    }
                                    if (tc->arguments && tc->arguments[0]) {
                                        /* Concatenate argument fragments */
                                        size_t old_len = pt->arguments ? strlen(pt->arguments) : 0;
                                        size_t add_len = strlen(tc->arguments);
                                        char *merged = realloc(pt->arguments, old_len + add_len + 1);
                                        if (merged) {
                                            memcpy(merged + old_len, tc->arguments, add_len);
                                            merged[old_len + add_len] = '\0';
                                            pt->arguments = merged;
                                        }
                                    }
                                }
                            }
                            message_free(delta);
                        }
                    }
                }
                line_start = line_end + 1;
                if (line_end >= end) break;
            }

            p = (*end == '\0') ? end : end + 2; /* Skip \n\n */
        }

sse_done:
        /* Set accumulated text on the message */
        if (text_buf) {
            accum->content = text_buf;
        }

        /* Flush merged tool calls into accum — only those with an id */
        for (int i = 0; i < n_pending; i++) {
            pending_tc_t *pt = &pending_tcs[i];
            if (pt->id && pt->id[0]) {
                message_add_tool_call(accum,
                    pt->id,
                    pt->name ? pt->name : "",
                    pt->arguments ? pt->arguments : "{}");
            }
            free(pt->id);
            free(pt->name);
            free(pt->arguments);
        }
        #undef MAX_PENDING_TC

        if (verbose) {
            fprintf(stderr, "[agent] SSE accumulated: content=%s, n_tool_calls=%d\n",
                    accum->content ? accum->content : "(nil)",
                    accum->n_tool_calls);
        }

        return accum;
    }

    /* Plain JSON — use provider's standard parser */
    if (verbose) {
        fprintf(stderr, "[agent] Detected plain JSON response\n");
    }
    return api->parse_response(body);
}

/* ── Non-streaming chat ─────────────────────────────────────────── */

message_t *agent_chat(agent_t *agent, const char *user_input) {
    if (!agent || !user_input) return NULL;

    agent_maybe_manage_context(agent);

    /* Add user message to conversation */
    message_t user_msg = {
        .role = MSG_ROLE_USER,
        .content = strdup(user_input),
        .n_tool_calls = 0,
        .n_tool_results = 0,
    };
    agent_add_message(agent, &user_msg);
    /* Don't free user_msg.content — it's now owned by agent->messages */

    message_t *final_response = NULL;
    int max_rounds = 10; /* Prevent infinite loops */

    while (max_rounds-- > 0) {
        /* Build request body */
        char *body = agent->api->build_request(agent);
        if (!body) {
            fprintf(stderr, "[agent] Failed to build request\n");
            break;
        }

        if (agent->verbose) {
            fprintf(stderr, "[agent] Request body: %s\n", body);
        }

        /* Build endpoint URL */
        char *url = agent_endpoint_url(agent);

        /* Build auth header */
        char *auth_val = agent_auth_header(agent);
        char *headers[4];
        int n_headers = 0;
        headers[n_headers] = malloc(strlen(agent->api->auth_header) +
                                     strlen(auth_val) + 4);
        sprintf(headers[n_headers++], "%s: %s", agent->api->auth_header, auth_val);
        headers[n_headers++] = strdup("Content-Type: application/json");

        /* Anthropic-specific version header */
        if (agent->api->type == PROVIDER_ANTHROPIC) {
            headers[n_headers++] = strdup("anthropic-version: 2023-06-01");
        }

        /* Build HTTP request */
        http_request_t req = {
            .method       = "POST",
            .url          = url,
            .headers      = headers,
            .header_count = n_headers,
            .body         = body,
            .body_length  = strlen(body),
            .timeout_ms   = 120000,
        };

        if (agent->verbose) {
            fprintf(stderr, "[agent] POST %s\n", url);
        }

        /* Send request, retrying transient failures (429/5xx/network) */
        http_response_t *resp = NULL;
        int max_retries = agent->provider.max_retries;
        if (max_retries < 0) max_retries = 0;
        int attempt = 0;

        while (1) {
            resp = http_request(&req);
            if (resp && resp->status_code >= 200 && resp->status_code < 300)
                break;

            int code = resp ? resp->status_code : 0;
            bool transient = !resp || http_status_transient(code);
            if (!transient || attempt >= max_retries) break;

            if (resp) http_response_free(resp);
            resp = NULL;
            agent->retry_count++;
            int delay = retry_delay_ms(attempt);
            fprintf(stderr,
                    "[agent] request failed (HTTP %d), retrying in %dms "
                    "(attempt %d/%d)\n",
                    code, delay, attempt + 1, max_retries);
            agent_sleep_ms(delay);
            attempt++;
        }

        /* Cleanup request data */
        free(body);
        free(url);
        free(auth_val);
        for (int i = 0; i < n_headers; i++) free(headers[i]);

        if (!resp) {
            fprintf(stderr, "[agent] HTTP request failed%s\n",
                    attempt > 0 ? " after retries" : "");
            break;
        }

        if (agent->verbose) {
            fprintf(stderr, "[agent] Response: status=%d, body_len=%zu\n",
                    resp->status_code, resp->body_length);
            /* Print raw response body (truncated if very long) */
            if (resp->body) {
                size_t show_len = resp->body_length;
                if (show_len > 2048) show_len = 2048;
                fprintf(stderr, "[agent] Raw body: %.*s%s\n",
                        (int)show_len, resp->body,
                        resp->body_length > 2048 ? "...(truncated)" : "");
            }
        }

        if (resp->status_code < 200 || resp->status_code >= 300) {
            fprintf(stderr, "[agent] API error (HTTP %d): %s\n",
                    resp->status_code,
                    resp->body ? resp->body : "(no body)");
            http_response_free(resp);
            break;
        }

        if (!resp->body) {
            fprintf(stderr, "[agent] Empty response body\n");
            http_response_free(resp);
            break;
        }

        /* Parse response */
        if (agent->verbose) {
            fprintf(stderr, "[agent] Parsing response (%zu bytes)...\n",
                    resp->body_length);
        }
        message_t *assistant_msg = agent_parse_response_body(
            agent->api, resp->body, agent->verbose, NULL, NULL);

        if (!assistant_msg) {
            /* Show full response body on parse failure for debugging */
            fprintf(stderr, "[agent] Failed to parse response. Raw body (%zu bytes):\n",
                    resp->body_length);
            fprintf(stderr, "[agent] --- BEGIN RAW RESPONSE ---\n");
            fprintf(stderr, "%s\n", resp->body);
            fprintf(stderr, "[agent] --- END RAW RESPONSE ---\n");
            http_response_free(resp);
            break;
        }

        /* Build API response summary for session logging */
        {
            json_value_t *summary = json_object();
            json_object_set(summary, "content",
                assistant_msg->content ? json_string(assistant_msg->content) : json_string(""));
            json_object_set(summary, "reasoning_content",
                assistant_msg->reasoning_content ? json_string(assistant_msg->reasoning_content) : json_string(""));
            assistant_msg->raw_response = json_stringify(summary);
            json_free(summary);
        }

        agent_record_usage(agent, assistant_msg);

        if (assistant_msg->n_tool_calls > 0) {
            if (agent->verbose) {
                fprintf(stderr, "[agent] Got %d tool call(s)\n",
                        assistant_msg->n_tool_calls);
            }

            /* Add assistant message to conversation once */
            agent_add_message(agent, assistant_msg);

            /* Execute tool calls (parallel where safe) */
            agent_execute_tools(agent, assistant_msg);

            message_free(assistant_msg);

            /* Loop back to send tool results to API */
            continue;
        }

        /* No tool calls — this is the final response */
        agent_add_message(agent, assistant_msg);

        /* Save the final response to return */
        final_response = message_copy(assistant_msg);
        message_free(assistant_msg);
        break;
    }

    if (max_rounds <= 0) {
        fprintf(stderr, "[agent] Max tool-use rounds exceeded\n");
    }

    if (!final_response) {
        final_response = message_create(MSG_ROLE_ASSISTANT,
            "Error: Failed to get a response from the API.");
    }

    return final_response;
}

/* ── Streaming chat ─────────────────────────────────────────────── */

/* Streaming receive context — passed to HTTP data callback */
typedef struct {
    sse_parser_t *parser;
    message_t *accum;
    api_provider_t *api;
    void (*on_token)(const char *, void *);
    void *token_ctx;
    char *text_buf;
    size_t text_len;
    char *reasoning_buf;  /* Accumulated reasoning_content deltas */
    size_t reasoning_len;
    bool verbose;
    /* Tool call merger */
    int ptc_index[16];
    char *ptc_id[16];
    char *ptc_name[16];
    char *ptc_args[16];
    int n_ptc;
} stream_rx_t;

static bool stream_sse_event(const sse_event_t *ev, void *p) {
    stream_rx_t *rx = (stream_rx_t *)p;
    if (!ev || !ev->data) return true;
    if (strcmp(ev->data, "[DONE]") == 0) return true;
    message_t *delta = rx->api->parse_chunk(ev->data);
    if (!delta) return true;
    if (delta->content) {
        size_t dl = strlen(delta->content);
        rx->text_buf = realloc(rx->text_buf, rx->text_len + dl + 1);
        memcpy(rx->text_buf + rx->text_len, delta->content, dl);
        rx->text_len += dl;
        rx->text_buf[rx->text_len] = '\0';
        if (rx->on_token) rx->on_token(delta->content, rx->token_ctx);
    }
    /* Accumulate reasoning_content (DeepSeek R1, OpenAI o1 thinking) */
    if (delta->reasoning_content) {
        size_t rl = strlen(delta->reasoning_content);
        rx->reasoning_buf = realloc(rx->reasoning_buf, rx->reasoning_len + rl + 1);
        memcpy(rx->reasoning_buf + rx->reasoning_len, delta->reasoning_content, rl);
        rx->reasoning_len += rl;
        rx->reasoning_buf[rx->reasoning_len] = '\0';
    }
    /* Usage from the final chunk (stream_options.include_usage) */
    if (delta->prompt_tokens > 0)
        rx->accum->prompt_tokens = delta->prompt_tokens;
    if (delta->completion_tokens > 0)
        rx->accum->completion_tokens = delta->completion_tokens;
    for (int i = 0; i < delta->n_tool_calls; i++) {
        int idx = delta->n_tool_calls > 1 ? i : 0;
        bool found = false;
        for (int j = 0; j < rx->n_ptc; j++) {
            if (rx->ptc_index[j] == idx) {
                found = true;
                if (delta->tool_calls[i].id && delta->tool_calls[i].id[0]) {
                    free(rx->ptc_id[j]); rx->ptc_id[j] = strdup(delta->tool_calls[i].id); }
                if (delta->tool_calls[i].name && delta->tool_calls[i].name[0]) {
                    free(rx->ptc_name[j]); rx->ptc_name[j] = strdup(delta->tool_calls[i].name); }
                if (delta->tool_calls[i].arguments && delta->tool_calls[i].arguments[0]) {
                    size_t ol = rx->ptc_args[j] ? strlen(rx->ptc_args[j]) : 0;
                    size_t al = strlen(delta->tool_calls[i].arguments);
                    /* If existing args end with } and new chunk starts with {,
                     * it's likely a duplicate/overlap — replace instead of append */
                    if (ol > 0 && rx->ptc_args[j][ol-1] == '}'
                        && delta->tool_calls[i].arguments[0] == '{') {
                        free(rx->ptc_args[j]);
                        rx->ptc_args[j] = strdup(delta->tool_calls[i].arguments);
                    } else {
                        rx->ptc_args[j] = realloc(rx->ptc_args[j], ol + al + 1);
                        memcpy(rx->ptc_args[j] + ol, delta->tool_calls[i].arguments, al);
                        rx->ptc_args[j][ol + al] = '\0';
                    }
                }
                break;
            }
        }
        if (!found && rx->n_ptc < 16) {
            int j = rx->n_ptc++;
            rx->ptc_index[j] = idx;
            rx->ptc_id[j] = delta->tool_calls[i].id ? strdup(delta->tool_calls[i].id) : NULL;
            rx->ptc_name[j] = delta->tool_calls[i].name ? strdup(delta->tool_calls[i].name) : NULL;
            rx->ptc_args[j] = delta->tool_calls[i].arguments ? strdup(delta->tool_calls[i].arguments) : NULL;
        }
    }
    message_free(delta);
    return true;
}

static void stream_rx_data(const char *data, size_t len, void *ctx) {
    stream_rx_t *rx = (stream_rx_t *)ctx;
    if (rx->verbose) {
        fprintf(stderr, "\n[stream] recv %zu bytes: %.*s%s\n",
                len, (int)(len < 800 ? len : 800), data,
                len > 800 ? "..." : "");
    }
    sse_parser_feed(rx->parser, data, len, stream_sse_event, rx);
}

message_t *agent_chat_stream(agent_t *agent, const char *user_input,
                             void (*on_token)(const char *token, void *ctx),
                             void *ctx) {
    if (!agent || !user_input) return NULL;

    agent_maybe_manage_context(agent);

    message_t user_msg = {
        .role = MSG_ROLE_USER, .content = strdup(user_input),
    };
    agent_add_message(agent, &user_msg);

    message_t *final_response = NULL;
    int max_rounds = 10;

    while (max_rounds-- > 0) {
        char *body = agent->api->build_request(agent);
        if (!body) { fprintf(stderr, "[agent] Failed to build request\n"); break; }
        if (agent->verbose) fprintf(stderr, "[agent] Request body: %s\n", body);

        char *url = agent_endpoint_url(agent);
        char *auth_val = agent_auth_header(agent);

        char *headers[4]; int n_headers = 0;
        headers[n_headers] = malloc(strlen(agent->api->auth_header) + strlen(auth_val) + 4);
        sprintf(headers[n_headers++], "%s: %s", agent->api->auth_header, auth_val);
        headers[n_headers++] = strdup("Content-Type: application/json");
        if (agent->api->type == PROVIDER_ANTHROPIC)
            headers[n_headers++] = strdup("anthropic-version: 2023-06-01");

        http_request_t req = {
            .method = "POST", .url = url, .headers = headers,
            .header_count = n_headers, .body = body, .body_length = strlen(body),
            .timeout_ms = 120000,
        };

        message_t *assistant_msg = NULL;

        if (agent->verbose) fprintf(stderr, "[agent] POST (stream) %s\n", url);

        /* Use real streaming HTTP if streaming is enabled */
        if (agent->provider.stream) {
            int max_retries = agent->provider.max_retries;
            if (max_retries < 0) max_retries = 0;
            int attempt = 0;
            assistant_msg = NULL;

            while (1) {
                stream_rx_t rx = {0};
                rx.parser = sse_parser_create();
                rx.accum = message_create(MSG_ROLE_ASSISTANT, NULL);
                rx.api = agent->api;
                rx.on_token = on_token;
                rx.token_ctx = ctx;
                rx.verbose = agent->verbose;

                http_response_t *resp = http_request_stream(&req, stream_rx_data, &rx);
                sse_parser_flush(rx.parser, NULL, NULL);

                if (rx.text_buf) rx.accum->content = rx.text_buf;
                if (rx.reasoning_buf) rx.accum->reasoning_content = rx.reasoning_buf;

                int code = resp ? resp->status_code : 0;
                bool ok = resp && code >= 200 && code < 300;
                if (!ok) {
                    /* Only retry before any output was emitted — retrying a
                     * partially-streamed response would duplicate output. */
                    bool emitted = rx.text_len > 0 || rx.reasoning_len > 0
                                   || rx.n_ptc > 0;
                    bool transient = !resp || http_status_transient(code);
                    bool can_retry = transient && !emitted && attempt < max_retries;

                    message_free(rx.accum);
                    sse_parser_free(rx.parser);
                    for (int i = 0; i < rx.n_ptc; i++) {
                        free(rx.ptc_id[i]); free(rx.ptc_name[i]);
                        free(rx.ptc_args[i]);
                    }
                    if (resp) http_response_free(resp);

                    if (!can_retry) {
                        if (resp) {
                            fprintf(stderr, "[agent] API error (HTTP %d)%s\n",
                                    code, emitted ? " (after partial output)" : "");
                        } else {
                            fprintf(stderr, "[agent] HTTP request failed%s\n",
                                    attempt > 0 ? " after retries" : "");
                        }
                        break;
                    }
                    agent->retry_count++;
                    int delay = retry_delay_ms(attempt);
                    fprintf(stderr,
                            "[agent] stream request failed (HTTP %d), "
                            "retrying in %dms (attempt %d/%d)\n",
                            code, delay, attempt + 1, max_retries);
                    agent_sleep_ms(delay);
                    attempt++;
                    continue;
                }
                http_response_free(resp);

                /* Build API response summary (content + reasoning_content) */
                {
                    json_value_t *summary = json_object();
                    json_object_set(summary, "content",
                        rx.accum->content ? json_string(rx.accum->content) : json_string(""));
                    json_object_set(summary, "reasoning_content",
                        rx.accum->reasoning_content ? json_string(rx.accum->reasoning_content) : json_string(""));
                    rx.accum->raw_response = json_stringify(summary);
                    json_free(summary);
                }
                /* Flush merged tool calls */
                for (int i = 0; i < rx.n_ptc; i++) {
                    if (rx.ptc_id[i] && rx.ptc_id[i][0])
                        message_add_tool_call(rx.accum, rx.ptc_id[i],
                            rx.ptc_name[i] ? rx.ptc_name[i] : "",
                            rx.ptc_args[i] ? rx.ptc_args[i] : "{}");
                    free(rx.ptc_id[i]); free(rx.ptc_name[i]); free(rx.ptc_args[i]);
                }
                sse_parser_free(rx.parser);
                assistant_msg = rx.accum;
                break;
            }
        } else {
            /* Fallback: non-streaming HTTP */
            http_response_t *resp = NULL;
            int max_retries = agent->provider.max_retries;
            if (max_retries < 0) max_retries = 0;
            int attempt = 0;

            while (1) {
                resp = http_request(&req);
                if (resp && resp->status_code >= 200 && resp->status_code < 300)
                    break;
                int code = resp ? resp->status_code : 0;
                bool transient = !resp || http_status_transient(code);
                if (!transient || attempt >= max_retries) break;
                if (resp) http_response_free(resp);
                resp = NULL;
                agent->retry_count++;
                int delay = retry_delay_ms(attempt);
                fprintf(stderr,
                        "[agent] request failed (HTTP %d), retrying in %dms "
                        "(attempt %d/%d)\n",
                        code, delay, attempt + 1, max_retries);
                agent_sleep_ms(delay);
                attempt++;
            }

            if (!resp || resp->status_code < 200 || resp->status_code >= 300) {
                if (resp) {
                    fprintf(stderr, "[agent] API error (HTTP %d): %s\n",
                            resp->status_code, resp->body ? resp->body : "");
                    http_response_free(resp);
                } else {
                    fprintf(stderr, "[agent] HTTP request failed%s\n",
                            attempt > 0 ? " after retries" : "");
                }
                assistant_msg = NULL;
            } else {
                assistant_msg = agent_parse_response_body(
                    agent->api, resp->body, agent->verbose, on_token, ctx);
                /* Build API response summary for session logging */
                if (assistant_msg) {
                    json_value_t *summary = json_object();
                    json_object_set(summary, "content",
                        assistant_msg->content ? json_string(assistant_msg->content) : json_string(""));
                    json_object_set(summary, "reasoning_content",
                        assistant_msg->reasoning_content ? json_string(assistant_msg->reasoning_content) : json_string(""));
                    assistant_msg->raw_response = json_stringify(summary);
                    json_free(summary);
                }
                http_response_free(resp);
            }
        }

        free(body); free(url); free(auth_val);
        for (int i = 0; i < n_headers; i++) free(headers[i]);

        if (!assistant_msg) break;

        agent_record_usage(agent, assistant_msg);

        /* Handle tool calls */
        if (assistant_msg->n_tool_calls > 0) {
            if (agent->verbose) {
                fprintf(stderr, "[agent] Got %d tool call(s)\n",
                        assistant_msg->n_tool_calls);
            }

            /* Streamed output doesn't include a trailing newline — end the
             * line so tool execution / the next round starts on a fresh line. */
            if (on_token && assistant_msg->content && assistant_msg->content[0]
                && assistant_msg->content[strlen(assistant_msg->content) - 1] != '\n') {
                on_token("\n", ctx);
            }

            /* Add assistant message once before tool results */
            agent_add_message(agent, assistant_msg);

            /* Execute tool calls (parallel where safe) */
            agent_execute_tools(agent, assistant_msg);

            message_free(assistant_msg);
            continue;
        }

        /* Final response */
        agent_add_message(agent, assistant_msg);
        final_response = message_copy(assistant_msg);
        message_free(assistant_msg);
        break;
    }

    if (!final_response) {
        final_response = message_create(MSG_ROLE_ASSISTANT,
            "Error: Failed to get a response from the API.");
    }

    return final_response;
}
