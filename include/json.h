/*
 * json.h — JSON abstraction API (wraps cJSON)
 */
#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

/* Opaque JSON value type */
typedef struct cJSON json_value_t;

/* ── Parsing / Serialization ─────────────────────────────────── */

json_value_t *json_parse(const char *str);
char         *json_stringify(const json_value_t *val);
char         *json_stringify_pretty(const json_value_t *val);
void          json_free(json_value_t *val);
json_value_t *json_dup(const json_value_t *val);

/* ── Constructors ────────────────────────────────────────────── */

json_value_t *json_object(void);
json_value_t *json_array(void);
json_value_t *json_string(const char *s);
json_value_t *json_number(double n);
json_value_t *json_bool(bool b);
json_value_t *json_null(void);

/* ── Object operations ───────────────────────────────────────── */

void          json_object_set(json_value_t *obj, const char *key, json_value_t *val);
json_value_t *json_object_get(const json_value_t *obj, const char *key);
json_value_t *json_object_get_str(const json_value_t *obj, const char *key);
bool          json_object_has(const json_value_t *obj, const char *key);
void          json_object_del(json_value_t *obj, const char *key);
int           json_object_size(const json_value_t *obj);

/* ── Array operations ────────────────────────────────────────── */

void          json_array_append(json_value_t *arr, json_value_t *val);
json_value_t *json_array_get(const json_value_t *arr, int index);
int           json_array_length(const json_value_t *arr);

/* ── Type checking ───────────────────────────────────────────── */

bool json_is_string(const json_value_t *val);
bool json_is_number(const json_value_t *val);
bool json_is_bool(const json_value_t *val);
bool json_is_object(const json_value_t *val);
bool json_is_array(const json_value_t *val);
bool json_is_null(const json_value_t *val);

/* ── Value extraction ────────────────────────────────────────── */

const char *json_string_value(const json_value_t *val);
double      json_number_value(const json_value_t *val);
bool        json_bool_value(const json_value_t *val);

/* ── Iteration helpers ───────────────────────────────────────── */

typedef struct {
    const json_value_t *obj;
    int pos;
} json_iter_t;

json_iter_t    json_iter_object(const json_value_t *obj);
bool           json_iter_next(json_iter_t *it, const char **key, json_value_t **val);

json_iter_t    json_iter_array(const json_value_t *arr);
bool           json_iter_next_arr(json_iter_t *it, json_value_t **val);

#endif /* JSON_H */
