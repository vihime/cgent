/*
 * todo.h — Session-scoped todo list for task planning
 *
 * The todo list lives for the process (one active session per process)
 * and is persisted in the session metadata. Tools let the model create,
 * update, and inspect the plan; the REPL shows it with /todos.
 */
#ifndef TODO_H
#define TODO_H

/* ── Todo store API ─────────────────────────────────────────────── */

typedef struct {
    char *content;
    char *status;   /* "pending", "in_progress", "completed", "cancelled" */
} todo_item_t;

/* Replace the whole todo list (copies items). */
void todo_replace(todo_item_t *items, int count);

/* Update one item by 0-based index. Returns 0 on success, -1 if the
 * index is out of range. */
int todo_update(int index, const char *status, const char *content);

/* Return a malloc'd snapshot of the current list (free with
 * todo_items_free). *count is set. */
todo_item_t *todo_snapshot(int *count);

/* Free an array returned by todo_snapshot. */
void todo_items_free(todo_item_t *items, int count);

/* Clear the list. */
void todo_clear(void);

/* Register the todo_* tools (todo_write, todo_update, todo_list). */
void todo_tools_register(void);

#endif /* TODO_H */
