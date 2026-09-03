#ifndef SNOVA_LSP_JSON_H
#define SNOVA_LSP_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonKind;

typedef struct JsonVal JsonVal;
typedef struct JsonMember JsonMember;

struct JsonMember {
    const char *key;
    JsonVal *val;
    JsonMember *next;
};

struct JsonVal {
    JsonKind kind;
    union {
        bool bool_val;
        double num_val;
        const char *str_val;
        struct {
            JsonVal **items;
            size_t len;
            size_t cap;
        } arr_val;
        struct {
            JsonMember *first;
            JsonMember *last;
            size_t count;
        } obj_val;
    };
};

/* Memory pool for JSON parsing */
typedef struct JsonPool JsonPool;

JsonPool *json_pool_create(size_t initial_cap);
void json_pool_destroy(JsonPool *pool);
void *json_pool_alloc(JsonPool *pool, size_t size);

/* Parser */
JsonVal *json_parse(JsonPool *pool, const char *text, size_t len, const char **err_out);

/* Query helpers */
const JsonVal *json_get(const JsonVal *obj, const char *key);
const char *json_get_str(const JsonVal *obj, const char *key, const char *def);
int64_t json_get_int(const JsonVal *obj, const char *key, int64_t def);
double json_get_num(const JsonVal *obj, const char *key, double def);
bool json_get_bool(const JsonVal *obj, const char *key, bool def);
const JsonVal *json_get_obj(const JsonVal *obj, const char *key);
const JsonVal *json_get_arr(const JsonVal *obj, const char *key);

size_t json_arr_len(const JsonVal *arr);
const JsonVal *json_arr_at(const JsonVal *arr, size_t index);

/* JSON String Builder */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool needs_comma;
} JsonBuilder;

void jb_init(JsonBuilder *jb);
void jb_free(JsonBuilder *jb);
char *jb_take(JsonBuilder *jb);

void jb_start_obj(JsonBuilder *jb);
void jb_end_obj(JsonBuilder *jb);
void jb_start_arr(JsonBuilder *jb);
void jb_end_arr(JsonBuilder *jb);

void jb_key(JsonBuilder *jb, const char *key);
void jb_str(JsonBuilder *jb, const char *val);
void jb_int(JsonBuilder *jb, int64_t val);
void jb_num(JsonBuilder *jb, double val);
void jb_bool(JsonBuilder *jb, bool val);
void jb_null(JsonBuilder *jb);
void jb_raw(JsonBuilder *jb, const char *raw_json);

/* Field shorthand helpers */
void jb_kv_str(JsonBuilder *jb, const char *key, const char *val);
void jb_kv_int(JsonBuilder *jb, const char *key, int64_t val);
void jb_kv_num(JsonBuilder *jb, const char *key, double val);
void jb_kv_bool(JsonBuilder *jb, const char *key, bool val);
void jb_kv_null(JsonBuilder *jb, const char *key);
void jb_kv_raw(JsonBuilder *jb, const char *key, const char *raw_json);

#endif /* SNOVA_LSP_JSON_H */
