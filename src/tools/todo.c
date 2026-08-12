/*
 * todo.c — Session-scoped todo list and todo_* tools
 *
 * Statuses follow the TodoWrite convention: pending, in_progress,
 * completed, cancelled. todo_write replaces the whole plan (this is
 * intentional — the model owns the plan), todo_update adjusts one item,
 * todo_list reads the current state.
 */
#include "todo.h"
#include "tools.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Store ──────────────────────────────────────────────────────── */

static todo_item_t *g_items = NULL;
static int g_count = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static bool valid_status(const char *s) {
    return s && (strcmp(s, "pending") == 0 ||
                 strcmp(s, "in_progress") == 0 ||
                 strcmp(s, "completed") == 0 ||
                 strcmp(s, "cancelled") == 0);
}

void todo_replace(todo_item_t *items, int count) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        free(g_items[i].content);
        free(g_items[i].status);
    }
    free(g_items);
    g_items = NULL;
    g_count = 0;

    if (count > 0) {
        g_items = calloc(count, sizeof(todo_item_t));
        for (int i = 0; i < count && g_items; i++) {
            g_items[i].content = strdup(items[i].content ? items[i].content : "");
            const char *st = valid_status(items[i].status)
                             ? items[i].status : "pending";
            g_items[i].status = strdup(st);
        }
        g_count = count;
    }
    pthread_mutex_unlock(&g_lock);
}

int todo_update(int index, const char *status, const char *content) {
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (index >= 0 && index < g_count) {
        if (content)
            g_items[index].content = strdup(content);
        if (status && valid_status(status))
            g_items[index].status = strdup(status);
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

todo_item_t *todo_snapshot(int *count) {
    pthread_mutex_lock(&g_lock);
    if (count) *count = g_count;
    todo_item_t *snap = calloc(g_count ? g_count : 1, sizeof(todo_item_t));
    for (int i = 0; i < g_count && snap; i++) {
        snap[i].content = strdup(g_items[i].content);
        snap[i].status = strdup(g_items[i].status);
    }
    pthread_mutex_unlock(&g_lock);
    return snap;
}

void todo_clear(void) {
    todo_replace(NULL, 0);
}

void todo_items_free(todo_item_t *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i].content);
        free(items[i].status);
    }
    free(items);
}

/* ── JSON helpers ───────────────────────────────────────────────── */

/* Append the current list to a JSON object as "todos". Caller owns obj. */
static void todos_to_json(json_value_t *obj) {
    json_value_t *arr = json_array();
    int n = 0;
    todo_item_t *items = todo_snapshot(&n);
    for (int i = 0; i < n; i++) {
        json_value_t *j = json_object();
        json_object_set(j, "index", json_number(i));
        json_object_set(j, "content", json_string(items[i].content));
        json_object_set(j, "status", json_string(items[i].status));
        json_array_append(arr, j);
    }
    todo_items_free(items, n);
    json_object_set(obj, "todos", arr);
    json_object_set(obj, "count", json_number(n));
}

/* Parse "todos": [...] into a todo_item_t array (caller frees). */
static int parse_todos(json_value_t *root, todo_item_t **out) {
    *out = NULL;
    json_value_t *arr = json_object_get(root, "todos");
    if (!arr || !json_is_array(arr)) return -1;
    int n = json_array_length(arr);
    todo_item_t *items = calloc(n, sizeof(todo_item_t));
    if (!items && n > 0) return -1;
    for (int i = 0; i < n; i++) {
        json_value_t *j = json_array_get(arr, i);
        json_value_t *content = json_object_get(j, "content");
        json_value_t *status = json_object_get(j, "status");
        items[i].content = strdup(content && json_is_string(content)
                                  ? json_string_value(content) : "");
        items[i].status = strdup(status && json_is_string(status)
                                 ? json_string_value(status) : "pending");
    }
    *out = items;
    return n;
}

/* ── Tool handlers ──────────────────────────────────────────────── */

static char *todo_result_ok(const char *msg) {
    json_value_t *out = json_object();
    json_object_set(out, "ok", json_bool(true));
    if (msg) json_object_set(out, "message", json_string(msg));
    todos_to_json(out);
    char *s = json_stringify(out);
    json_free(out);
    return s;
}

static char *tool_todo_write(const char *name, const char *args_json,
                             char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }
    todo_item_t *items = NULL;
    int n = parse_todos(args, &items);
    if (n < 0) {
        if (error) *error = strdup("Missing or invalid 'todos' array");
        json_free(args);
        return NULL;
    }
    todo_replace(items, n);
    todo_items_free(items, n);
    json_free(args);
    return todo_result_ok("Todo list updated");
}

static char *tool_todo_update(const char *name, const char *args_json,
                              char **error) {
    (void)name;
    json_value_t *args = json_parse(args_json);
    if (!args) {
        if (error) *error = strdup("Invalid JSON arguments");
        return NULL;
    }
    json_value_t *idx_val = json_object_get(args, "index");
    json_value_t *status_val = json_object_get(args, "status");
    json_value_t *content_val = json_object_get(args, "content");
    if (!idx_val || !json_is_number(idx_val)) {
        if (error) *error = strdup("Missing numeric 'index' argument");
        json_free(args);
        return NULL;
    }
    if ((!status_val || !json_is_string(status_val)) &&
        (!content_val || !json_is_string(content_val))) {
        if (error) *error = strdup("Provide 'status' and/or 'content'");
        json_free(args);
        return NULL;
    }

    int index = (int)json_number_value(idx_val);
    const char *status = status_val && json_is_string(status_val)
                         ? json_string_value(status_val) : NULL;
    const char *content = content_val && json_is_string(content_val)
                          ? json_string_value(content_val) : NULL;
    if (status && strcmp(status, "in-progress") == 0)
        status = "in_progress";
    if (status && strcmp(status, "in_progress") != 0 &&
        strcmp(status, "pending") != 0 &&
        strcmp(status, "completed") != 0 &&
        strcmp(status, "cancelled") != 0) {
        if (error) *error = strdup(
            "Invalid status (use pending, in_progress, completed, cancelled)");
        json_free(args);
        return NULL;
    }

    if (todo_update(index, status, content) != 0) {
        if (error) *error = strdup("Index out of range");
        json_free(args);
        return NULL;
    }
    json_free(args);
    return todo_result_ok("Todo updated");
}

static char *tool_todo_list(const char *name, const char *args_json,
                            char **error) {
    (void)name; (void)args_json; (void)error;
    return todo_result_ok(NULL);
}

/* ── Registration ───────────────────────────────────────────────── */

void todo_tools_register(void) {
    tool_t *t;

    t = tool_create("todo_write",
        "Replace the entire todo list with a new plan. Each item has "
        "'content' (the task) and 'status': pending, in_progress, "
        "completed, or cancelled. Call this at the start of a multi-step "
        "task to lay out your plan, then update items as you progress.",
        "{\"type\":\"object\",\"properties\":{"
        "\"todos\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
        "\"properties\":{\"content\":{\"type\":\"string\"},"
        "\"status\":{\"type\":\"string\",\"enum\":[\"pending\","
        "\"in_progress\",\"completed\",\"cancelled\"]}},"
        "\"required\":[\"content\"]}}},"
        "\"required\":[\"todos\"]}",
        tool_todo_write);
    t->thread_safe = true;
    tool_registry_add(t);

    t = tool_create("todo_update",
        "Update a single todo item by its 0-based index: change its "
        "status and/or rewrite its content.",
        "{\"type\":\"object\",\"properties\":{"
        "\"index\":{\"type\":\"integer\",\"description\":\"0-based item index\"},"
        "\"status\":{\"type\":\"string\",\"enum\":[\"pending\","
        "\"in_progress\",\"completed\",\"cancelled\"]},"
        "\"content\":{\"type\":\"string\",\"description\":\"New task text\"}},"
        "\"required\":[\"index\"]}",
        tool_todo_update);
    t->thread_safe = true;
    tool_registry_add(t);

    t = tool_create("todo_list",
        "Return the current todo list with each item's index, content, "
        "and status. Use this to review what remains before continuing.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_todo_list);
    t->thread_safe = true;
    tool_registry_add(t);
}
