/*
 * test_json.c — JSON wrapper unit tests
 */
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests = 0, passed = 0;

#define TEST(name) do { tests++; printf("  %-50s", name); fflush(stdout); } while(0)
#define OK() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("assertion failed: %s", #cond); return; } } while(0)

static void test_parse_basic(void) {
    TEST("parse basic object");
    const char *s = "{\"key\":\"value\",\"num\":42}";
    json_value_t *obj = json_parse(s);
    CHECK(obj != NULL);
    CHECK(json_is_object(obj));
    json_value_t *v = json_object_get(obj, "key");
    CHECK(v != NULL);
    CHECK(strcmp(json_string_value(v), "value") == 0);
    json_free(obj);
    OK();
}

static void test_stringify(void) {
    TEST("stringify round-trip");
    json_value_t *obj = json_object();
    json_object_set(obj, "hello", json_string("world"));
    json_object_set(obj, "count", json_number(123));
    char *s = json_stringify(obj);
    CHECK(s != NULL);
    json_value_t *parsed = json_parse(s);
    CHECK(parsed != NULL);
    json_value_t *v = json_object_get(parsed, "hello");
    CHECK(v != NULL);
    CHECK(strcmp(json_string_value(v), "world") == 0);
    free(s);
    json_free(obj);
    json_free(parsed);
    OK();
}

static void test_array(void) {
    TEST("array operations");
    json_value_t *arr = json_array();
    json_array_append(arr, json_string("a"));
    json_array_append(arr, json_string("b"));
    json_array_append(arr, json_number(3));
    CHECK(json_array_length(arr) == 3);
    CHECK(strcmp(json_string_value(json_array_get(arr, 0)), "a") == 0);
    CHECK(json_number_value(json_array_get(arr, 2)) == 3.0);
    json_free(arr);
    OK();
}

static void test_nested(void) {
    TEST("nested structures");
    json_value_t *outer = json_object();
    json_value_t *inner = json_object();
    json_object_set(inner, "x", json_number(1));
    json_object_set(outer, "inner", inner);
    json_value_t *arr = json_array();
    json_array_append(arr, json_bool(true));
    json_array_append(arr, json_bool(false));
    json_object_set(outer, "flags", arr);

    char *s = json_stringify(outer);
    json_value_t *parsed = json_parse(s);
    CHECK(parsed != NULL);
    json_value_t *v = json_object_get(json_object_get(parsed, "inner"), "x");
    CHECK(v != NULL);
    CHECK(json_number_value(v) == 1.0);
    json_free(outer);
    json_free(parsed);
    free(s);
    OK();
}

static void test_null_and_bool(void) {
    TEST("null and bool");
    json_value_t *obj = json_object();
    json_object_set(obj, "nothing", json_null());
    json_object_set(obj, "yes", json_bool(true));
    json_object_set(obj, "no", json_bool(false));
    CHECK(json_is_null(json_object_get(obj, "nothing")));
    CHECK(json_bool_value(json_object_get(obj, "yes")) == true);
    CHECK(json_bool_value(json_object_get(obj, "no")) == false);
    json_free(obj);
    OK();
}

static void test_parse_invalid(void) {
    TEST("parse invalid JSON");
    json_value_t *v = json_parse("{bad");
    CHECK(v == NULL);
    OK();
}

int main(void) {
    printf("JSON tests:\n");
    test_parse_basic();
    test_stringify();
    test_array();
    test_nested();
    test_null_and_bool();
    test_parse_invalid();
    printf("  %d/%d passed\n", passed, tests);
    return passed == tests ? 0 : 1;
}
