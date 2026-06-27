/*
 * core.h — Agent, Message, Tool core data structures
 */
#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stddef.h>

/* ── Message ────────────────────────────────────────────────────── */

typedef enum {
    MSG_ROLE_SYSTEM,
    MSG_ROLE_USER,
    MSG_ROLE_ASSISTANT,
    MSG_ROLE_TOOL
} message_role_t;

/* A single tool call from the assistant */
typedef struct {
    char *id;           /* Tool call ID */
    char *name;         /* Function name */
    char *arguments;    /* JSON string of arguments */
} tool_call_t;

/* A single tool result (returned to the model) */
typedef struct {
    char *tool_call_id; /* Which tool call this answers */
    char *content;      /* Tool output content */
    bool is_error;      /* Whether the tool execution errored */
} tool_result_t;

/* A conversation message */
typedef struct {
    message_role_t role;
    char *content;              /* Text content (nullable for tool calls) */
    char *name;                 /* Optional name for the message sender */
    tool_call_t *tool_calls;    /* Array (assistant role) */
    int n_tool_calls;
    tool_result_t *tool_results; /* Array (tool role) */
    int n_tool_results;
} message_t;

/* ── Tool ───────────────────────────────────────────────────────── */

/* Tool handler: takes tool name + JSON arguments, returns JSON result.
   On error, set *error to an error string and return NULL. */
typedef char *(*tool_handler_t)(const char *tool_name,
                                const char *args_json,
                                char **error);

typedef struct {
    char *name;
    char *description;
    char *parameters_schema;    /* JSON Schema for the function parameters */
    tool_handler_t handler;     /* Execution function */
} tool_t;

/* ── Provider config ────────────────────────────────────────────── */

typedef struct {
    char *api_key;
    char *base_url;
    char *model;
    double temperature;
    int max_tokens;
    bool stream;
} provider_config_t;

/* ── Agent ──────────────────────────────────────────────────────── */

typedef struct agent agent_t;

/* Forward declaration for the provider */
struct api_provider;

struct agent {
    provider_config_t provider;
    struct api_provider *api;   /* Bound provider interface */

    /* Conversation history */
    message_t *messages;
    int n_messages;
    int cap_messages;

    /* Registered tools */
    tool_t *tools;
    int n_tools;
    int cap_tools;

    /* System prompt */
    char *system_prompt;

    /* Settings */
    bool verbose;
};

/* ── Message lifecycle ──────────────────────────────────────────── */

message_t *message_create(message_role_t role, const char *content);
/* Free a standalone message (including the struct itself) */
void       message_free(message_t *msg);
/* Clear message contents without freeing the struct (for array elements) */
void       message_clear(message_t *msg);
message_t *message_copy(const message_t *msg);

/* Add a tool call to an assistant message */
void       message_add_tool_call(message_t *msg, const char *id,
                                 const char *name, const char *args);
/* Add a tool result to a tool message */
void       message_add_tool_result(message_t *msg, const char *call_id,
                                   const char *content, bool is_error);

/* ── Tool lifecycle ─────────────────────────────────────────────── */

tool_t *tool_create(const char *name, const char *description,
                    const char *parameters_schema, tool_handler_t handler);
void    tool_free(tool_t *tool);

/* ── Agent lifecycle ────────────────────────────────────────────── */

/* Create an agent bound to a provider */
agent_t *agent_create(provider_config_t *config, struct api_provider *api);
void     agent_free(agent_t *agent);

/* Set the system prompt */
void     agent_set_system_prompt(agent_t *agent, const char *prompt);

/* Register a tool */
int      agent_add_tool(agent_t *agent, const tool_t *tool);

/* Append a message to the conversation */
int      agent_add_message(agent_t *agent, const message_t *msg);

/* ── Agent operations ───────────────────────────────────────────── */

/* Run a single round of chat (user input → assistant response).
 * Automatically handles the tool-use loop.
 * Returns the final assistant message. Caller must free. */
message_t *agent_chat(agent_t *agent, const char *user_input);

/* Streaming variant. on_token called for each text delta.
 * Returns the complete final message. */
message_t *agent_chat_stream(agent_t *agent, const char *user_input,
                             void (*on_token)(const char *token, void *ctx),
                             void *ctx);

#endif /* CORE_H */
