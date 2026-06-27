/*
 * json_wrapper.c — JSON API wrapping cJSON
 */
#include "json.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* ── Parsing / Serialization ─────────────────────────────────── */

json_value_t *json_parse(const char *str) {
    return cJSON_Parse(str);
}

char *json_stringify(const json_value_t *val) {
    return cJSON_PrintUnformatted(val);
}

char *json_stringify_pretty(const json_value_t *val) {
    return cJSON_Print(val);
}

void json_free(json_value_t *val) {
    cJSON_Delete(val);
}

json_value_t *json_dup(const json_value_t *val) {
    return cJSON_Duplicate(val, 1);
}

/* ── Constructors ────────────────────────────────────────────── */

json_value_t *json_object(void)  { return cJSON_CreateObject(); }
json_value_t *json_array(void)   { return cJSON_CreateArray(); }
json_value_t *json_string(const char *s)   { return cJSON_CreateString(s); }
json_value_t *json_number(double n)        { return cJSON_CreateNumber(n); }
json_value_t *json_bool(bool b)            { return cJSON_CreateBool(b); }
json_value_t *json_null(void)              { return cJSON_CreateNull(); }

/* ── Object operations ───────────────────────────────────────── */

void json_object_set(json_value_t *obj, const char *key, json_value_t *val) {
    cJSON_AddItemToObject(obj, key, val);
}

json_value_t *json_object_get(const json_value_t *obj, const char *key) {
    return cJSON_GetObjectItem(obj, key);
}

json_value_t *json_object_get_str(const json_value_t *obj, const char *key) {
    return cJSON_GetObjectItem(obj, key);
}

bool json_object_has(const json_value_t *obj, const char *key) {
    return cJSON_HasObjectItem(obj, key);
}

void json_object_del(json_value_t *obj, const char *key) {
    cJSON_DeleteItemFromObject(obj, key);
}

int json_object_size(const json_value_t *obj) {
    return cJSON_GetArraySize(obj);
}

/* ── Array operations ────────────────────────────────────────── */

void json_array_append(json_value_t *arr, json_value_t *val) {
    cJSON_AddItemToArray(arr, val);
}

json_value_t *json_array_get(const json_value_t *arr, int index) {
    return cJSON_GetArrayItem(arr, index);
}

int json_array_length(const json_value_t *arr) {
    return cJSON_GetArraySize(arr);
}

/* ── Type checking ───────────────────────────────────────────── */

bool json_is_string(const json_value_t *val) { return cJSON_IsString(val); }
bool json_is_number(const json_value_t *val) { return cJSON_IsNumber(val); }
bool json_is_bool(const json_value_t *val)   { return cJSON_IsBool(val); }
bool json_is_object(const json_value_t *val) { return cJSON_IsObject(val); }
bool json_is_array(const json_value_t *val)  { return cJSON_IsArray(val); }
bool json_is_null(const json_value_t *val)   { return cJSON_IsNull(val); }

/* ── Value extraction ────────────────────────────────────────── */

const char *json_string_value(const json_value_t *val) {
    return cJSON_GetStringValue(val);
}

double json_number_value(const json_value_t *val) {
    return cJSON_GetNumberValue(val);
}

bool json_bool_value(const json_value_t *val) {
    return cJSON_IsTrue(val);
}

/* ── Iteration helpers ───────────────────────────────────────── */

json_iter_t json_iter_object(const json_value_t *obj) {
    json_iter_t it = { .obj = obj, .pos = 0 };
    (void)it;
    return it;
}

bool json_iter_next(json_iter_t *it, const char **key, json_value_t **val) {
    /* cJSON doesn't have a simple iterator; use array index on object */
    if (!it || !it->obj) return false;
    cJSON *child = cJSON_GetArrayItem(it->obj, it->pos);
    if (!child) return false;
    if (key) *key = child->string;
    if (val) *val = child;
    it->pos++;
    return true;
}

json_iter_t json_iter_array(const json_value_t *arr) {
    json_iter_t it = { .obj = arr, .pos = 0 };
    return it;
}

bool json_iter_next_arr(json_iter_t *it, json_value_t **val) {
    if (!it || !it->obj) return false;
    cJSON *child = cJSON_GetArrayItem(it->obj, it->pos);
    if (!child) return false;
    if (val) *val = child;
    it->pos++;
    return true;
}
