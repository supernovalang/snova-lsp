#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

struct JsonPoolChunk {
    struct JsonPoolChunk *next;
    size_t cap;
    size_t used;
    char data[];
};

struct JsonPool {
    struct JsonPoolChunk *head;
    size_t default_chunk_cap;
};

JsonPool *json_pool_create(size_t initial_cap) {
    if (initial_cap < 4096) initial_cap = 4096;
    JsonPool *pool = (JsonPool *)malloc(sizeof(JsonPool));
    if (!pool) return NULL;
    pool->default_chunk_cap = initial_cap;
    struct JsonPoolChunk *chunk = (struct JsonPoolChunk *)malloc(sizeof(struct JsonPoolChunk) + initial_cap);
    if (!chunk) {
        free(pool);
        return NULL;
    }
    chunk->next = NULL;
    chunk->cap = initial_cap;
    chunk->used = 0;
    pool->head = chunk;
    return pool;
}

void json_pool_destroy(JsonPool *pool) {
    if (!pool) return;
    struct JsonPoolChunk *cur = pool->head;
    while (cur) {
        struct JsonPoolChunk *next = cur->next;
        free(cur);
        cur = next;
    }
    free(pool);
}

void *json_pool_alloc(JsonPool *pool, size_t size) {
    size = (size + 7) & ~7ULL; // 8-byte align
    if (pool->head->used + size <= pool->head->cap) {
        void *ptr = pool->head->data + pool->head->used;
        pool->head->used += size;
        return ptr;
    }
    size_t chunk_cap = pool->default_chunk_cap;
    if (size > chunk_cap) chunk_cap = size + 4096;
    struct JsonPoolChunk *chunk = (struct JsonPoolChunk *)malloc(sizeof(struct JsonPoolChunk) + chunk_cap);
    if (!chunk) return NULL;
    chunk->cap = chunk_cap;
    chunk->used = size;
    chunk->next = pool->head;
    pool->head = chunk;
    return chunk->data;
}

/* Parser State */
typedef struct {
    JsonPool *pool;
    const char *src;
    size_t len;
    size_t pos;
    const char *err;
} JsonParser;

static void skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char peek_char(JsonParser *p) {
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static char next_char(JsonParser *p) {
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static JsonVal *parse_value(JsonParser *p);

static char *parse_string_raw(JsonParser *p) {
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        p->err = "Expected '\"'";
        return NULL;
    }
    p->pos++; // skip opening quote
    size_t start = p->pos;
    size_t out_len = 0;
    
    // First pass: compute length and check validity
    size_t cur = start;
    bool has_escape = false;
    while (cur < p->len && p->src[cur] != '"') {
        if (p->src[cur] == '\\') {
            has_escape = true;
            cur++;
            if (cur >= p->len) {
                p->err = "Unterminated escape sequence in string";
                return NULL;
            }
        }
        cur++;
        out_len++;
    }
    if (cur >= p->len) {
        p->err = "Unterminated string literal";
        return NULL;
    }

    char *str = (char *)json_pool_alloc(p->pool, out_len + 1);
    if (!str) {
        p->err = "Out of memory in string parse";
        return NULL;
    }

    if (!has_escape) {
        memcpy(str, p->src + start, out_len);
        str[out_len] = '\0';
        p->pos = cur + 1;
        return str;
    }

    // Second pass with escape decoding
    size_t w = 0;
    cur = start;
    while (cur < p->len && p->src[cur] != '"') {
        if (p->src[cur] == '\\') {
            cur++;
            char esc = p->src[cur++];
            switch (esc) {
                case '"':  str[w++] = '"'; break;
                case '\\': str[w++] = '\\'; break;
                case '/':  str[w++] = '/'; break;
                case 'b':  str[w++] = '\b'; break;
                case 'f':  str[w++] = '\f'; break;
                case 'n':  str[w++] = '\n'; break;
                case 'r':  str[w++] = '\r'; break;
                case 't':  str[w++] = '\t'; break;
                case 'u': {
                    // 4 hex digits
                    if (cur + 4 <= p->len) {
                        uint32_t codepoint = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = p->src[cur++];
                            codepoint <<= 4;
                            if (h >= '0' && h <= '9') codepoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (h - 'A' + 10);
                        }
                        if (codepoint < 0x80) {
                            str[w++] = (char)codepoint;
                        } else if (codepoint < 0x800) {
                            str[w++] = (char)(0xC0 | (codepoint >> 6));
                            str[w++] = (char)(0x80 | (codepoint & 0x3F));
                        } else {
                            str[w++] = (char)(0xE0 | (codepoint >> 12));
                            str[w++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            str[w++] = (char)(0x80 | (codepoint & 0x3F));
                        }
                    } else {
                        str[w++] = '?';
                    }
                    break;
                }
                default:
                    str[w++] = esc;
                    break;
            }
        } else {
            str[w++] = p->src[cur++];
        }
    }
    str[w] = '\0';
    p->pos = cur + 1; // skip closing quote
    return str;
}

static JsonVal *parse_object(JsonParser *p) {
    p->pos++; // skip '{'
    JsonVal *obj = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
    if (!obj) return NULL;
    obj->kind = JSON_OBJECT;
    obj->obj_val.first = NULL;
    obj->obj_val.last = NULL;
    obj->obj_val.count = 0;

    skip_ws(p);
    if (peek_char(p) == '}') {
        p->pos++;
        return obj;
    }

    while (p->pos < p->len) {
        skip_ws(p);
        if (peek_char(p) != '"') {
            p->err = "Expected string key in object";
            return NULL;
        }
        char *key = parse_string_raw(p);
        if (!key) return NULL;

        skip_ws(p);
        if (next_char(p) != ':') {
            p->err = "Expected ':' after object key";
            return NULL;
        }

        JsonVal *val = parse_value(p);
        if (!val) return NULL;

        JsonMember *mem = (JsonMember *)json_pool_alloc(p->pool, sizeof(JsonMember));
        if (!mem) return NULL;
        mem->key = key;
        mem->val = val;
        mem->next = NULL;

        if (!obj->obj_val.first) {
            obj->obj_val.first = mem;
            obj->obj_val.last = mem;
        } else {
            obj->obj_val.last->next = mem;
            obj->obj_val.last = mem;
        }
        obj->obj_val.count++;

        skip_ws(p);
        char sep = peek_char(p);
        if (sep == ',') {
            p->pos++;
            skip_ws(p);
            // Allow trailing comma
            if (peek_char(p) == '}') {
                p->pos++;
                break;
            }
        } else if (sep == '}') {
            p->pos++;
            break;
        } else {
            p->err = "Expected ',' or '}' in object";
            return NULL;
        }
    }
    return obj;
}

static JsonVal *parse_array(JsonParser *p) {
    p->pos++; // skip '['
    JsonVal *arr = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
    if (!arr) return NULL;
    arr->kind = JSON_ARRAY;
    arr->arr_val.len = 0;
    arr->arr_val.cap = 8;
    arr->arr_val.items = (JsonVal **)json_pool_alloc(p->pool, sizeof(JsonVal *) * arr->arr_val.cap);

    skip_ws(p);
    if (peek_char(p) == ']') {
        p->pos++;
        return arr;
    }

    while (p->pos < p->len) {
        JsonVal *item = parse_value(p);
        if (!item) return NULL;

        if (arr->arr_val.len >= arr->arr_val.cap) {
            size_t new_cap = arr->arr_val.cap * 2;
            JsonVal **new_items = (JsonVal **)json_pool_alloc(p->pool, sizeof(JsonVal *) * new_cap);
            memcpy(new_items, arr->arr_val.items, sizeof(JsonVal *) * arr->arr_val.len);
            arr->arr_val.items = new_items;
            arr->arr_val.cap = new_cap;
        }
        arr->arr_val.items[arr->arr_val.len++] = item;

        skip_ws(p);
        char sep = peek_char(p);
        if (sep == ',') {
            p->pos++;
            skip_ws(p);
            if (peek_char(p) == ']') {
                p->pos++;
                break;
            }
        } else if (sep == ']') {
            p->pos++;
            break;
        } else {
            p->err = "Expected ',' or ']' in array";
            return NULL;
        }
    }
    return arr;
}

static JsonVal *parse_number(JsonParser *p) {
    size_t start = p->pos;
    if (p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    char tmp[64];
    size_t num_len = p->pos - start;
    if (num_len >= sizeof(tmp)) num_len = sizeof(tmp) - 1;
    memcpy(tmp, p->src + start, num_len);
    tmp[num_len] = '\0';

    JsonVal *val = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
    if (!val) return NULL;
    val->kind = JSON_NUMBER;
    val->num_val = strtod(tmp, NULL);
    return val;
}

static JsonVal *parse_value(JsonParser *p) {
    skip_ws(p);
    if (p->pos >= p->len) {
        p->err = "Unexpected end of input";
        return NULL;
    }

    char c = p->src[p->pos];
    if (c == '{') {
        return parse_object(p);
    } else if (c == '[') {
        return parse_array(p);
    } else if (c == '"') {
        char *str = parse_string_raw(p);
        if (!str) return NULL;
        JsonVal *val = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
        if (!val) return NULL;
        val->kind = JSON_STRING;
        val->str_val = str;
        return val;
    } else if (c == 't' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JsonVal *val = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
        if (!val) return NULL;
        val->kind = JSON_BOOL;
        val->bool_val = true;
        return val;
    } else if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JsonVal *val = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
        if (!val) return NULL;
        val->kind = JSON_BOOL;
        val->bool_val = false;
        return val;
    } else if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        JsonVal *val = (JsonVal *)json_pool_alloc(p->pool, sizeof(JsonVal));
        if (!val) return NULL;
        val->kind = JSON_NULL;
        return val;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }

    p->err = "Invalid character";
    return NULL;
}

JsonVal *json_parse(JsonPool *pool, const char *text, size_t len, const char **err_out) {
    if (!text || len == 0) {
        if (err_out) *err_out = "Empty input";
        return NULL;
    }
    JsonParser parser = {
        .pool = pool,
        .src = text,
        .len = len,
        .pos = 0,
        .err = NULL
    };
    JsonVal *root = parse_value(&parser);
    if (!root && err_out) {
        *err_out = parser.err ? parser.err : "JSON parse error";
    }
    return root;
}

const JsonVal *json_get(const JsonVal *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT || !key) return NULL;
    for (JsonMember *m = obj->obj_val.first; m; m = m->next) {
        if (strcmp(m->key, key) == 0) {
            return m->val;
        }
    }
    return NULL;
}

const char *json_get_str(const JsonVal *obj, const char *key, const char *def) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_STRING) return v->str_val;
    return def;
}

int64_t json_get_int(const JsonVal *obj, const char *key, int64_t def) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_NUMBER) return (int64_t)v->num_val;
    return def;
}

double json_get_num(const JsonVal *obj, const char *key, double def) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_NUMBER) return v->num_val;
    return def;
}

bool json_get_bool(const JsonVal *obj, const char *key, bool def) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_BOOL) return v->bool_val;
    return def;
}

const JsonVal *json_get_obj(const JsonVal *obj, const char *key) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_OBJECT) return v;
    return NULL;
}

const JsonVal *json_get_arr(const JsonVal *obj, const char *key) {
    const JsonVal *v = json_get(obj, key);
    if (v && v->kind == JSON_ARRAY) return v;
    return NULL;
}

size_t json_arr_len(const JsonVal *arr) {
    if (!arr || arr->kind != JSON_ARRAY) return 0;
    return arr->arr_val.len;
}

const JsonVal *json_arr_at(const JsonVal *arr, size_t index) {
    if (!arr || arr->kind != JSON_ARRAY || index >= arr->arr_val.len) return NULL;
    return arr->arr_val.items[index];
}

/* JSON String Builder */

void jb_init(JsonBuilder *jb) {
    jb->cap = 256;
    jb->len = 0;
    jb->buf = (char *)malloc(jb->cap);
    if (jb->buf) jb->buf[0] = '\0';
    jb->needs_comma = false;
}

void jb_free(JsonBuilder *jb) {
    if (jb->buf) {
        free(jb->buf);
        jb->buf = NULL;
    }
    jb->len = 0;
    jb->cap = 0;
}

char *jb_take(JsonBuilder *jb) {
    char *res = jb->buf;
    jb->buf = NULL;
    jb->len = 0;
    jb->cap = 0;
    return res;
}

static void jb_ensure(JsonBuilder *jb, size_t extra) {
    if (jb->len + extra + 1 > jb->cap) {
        size_t new_cap = jb->cap * 2;
        if (new_cap < jb->len + extra + 1) new_cap = jb->len + extra + 1 + 256;
        char *new_buf = (char *)realloc(jb->buf, new_cap);
        if (new_buf) {
            jb->buf = new_buf;
            jb->cap = new_cap;
        }
    }
}

static void jb_putc(JsonBuilder *jb, char c) {
    jb_ensure(jb, 1);
    jb->buf[jb->len++] = c;
    jb->buf[jb->len] = '\0';
}

static void jb_puts(JsonBuilder *jb, const char *s) {
    if (!s) return;
    size_t slen = strlen(s);
    jb_ensure(jb, slen);
    memcpy(jb->buf + jb->len, s, slen);
    jb->len += slen;
    jb->buf[jb->len] = '\0';
}

static void jb_maybe_comma(JsonBuilder *jb) {
    if (jb->needs_comma) {
        jb_putc(jb, ',');
        jb->needs_comma = false;
    }
}

void jb_start_obj(JsonBuilder *jb) {
    jb_maybe_comma(jb);
    jb_putc(jb, '{');
    jb->needs_comma = false;
}

void jb_end_obj(JsonBuilder *jb) {
    jb_putc(jb, '}');
    jb->needs_comma = true;
}

void jb_start_arr(JsonBuilder *jb) {
    jb_maybe_comma(jb);
    jb_putc(jb, '[');
    jb->needs_comma = false;
}

void jb_end_arr(JsonBuilder *jb) {
    jb_putc(jb, ']');
    jb->needs_comma = true;
}

void jb_key(JsonBuilder *jb, const char *key) {
    jb_maybe_comma(jb);
    jb_str(jb, key);
    jb_putc(jb, ':');
    jb->needs_comma = false;
}

void jb_str(JsonBuilder *jb, const char *val) {
    if (!val) {
        jb_null(jb);
        return;
    }
    jb_maybe_comma(jb);
    jb_putc(jb, '"');
    for (const char *p = val; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  jb_puts(jb, "\\\""); break;
            case '\\': jb_puts(jb, "\\\\"); break;
            case '\b': jb_puts(jb, "\\b"); break;
            case '\f': jb_puts(jb, "\\f"); break;
            case '\n': jb_puts(jb, "\\n"); break;
            case '\r': jb_puts(jb, "\\r"); break;
            case '\t': jb_puts(jb, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jb_puts(jb, esc);
                } else {
                    jb_putc(jb, (char)c);
                }
                break;
        }
    }
    jb_putc(jb, '"');
    jb->needs_comma = true;
}

void jb_int(JsonBuilder *jb, int64_t val) {
    jb_maybe_comma(jb);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    jb_puts(jb, buf);
    jb->needs_comma = true;
}

void jb_num(JsonBuilder *jb, double val) {
    jb_maybe_comma(jb);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", val);
    jb_puts(jb, buf);
    jb->needs_comma = true;
}

void jb_bool(JsonBuilder *jb, bool val) {
    jb_maybe_comma(jb);
    jb_puts(jb, val ? "true" : "false");
    jb->needs_comma = true;
}

void jb_null(JsonBuilder *jb) {
    jb_maybe_comma(jb);
    jb_puts(jb, "null");
    jb->needs_comma = true;
}

void jb_raw(JsonBuilder *jb, const char *raw_json) {
    if (!raw_json) return;
    jb_maybe_comma(jb);
    jb_puts(jb, raw_json);
    jb->needs_comma = true;
}

void jb_kv_str(JsonBuilder *jb, const char *key, const char *val) {
    jb_key(jb, key);
    jb_str(jb, val);
}

void jb_kv_int(JsonBuilder *jb, const char *key, int64_t val) {
    jb_key(jb, key);
    jb_int(jb, val);
}

void jb_kv_num(JsonBuilder *jb, const char *key, double val) {
    jb_key(jb, key);
    jb_num(jb, val);
}

void jb_kv_bool(JsonBuilder *jb, const char *key, bool val) {
    jb_key(jb, key);
    jb_bool(jb, val);
}

void jb_kv_null(JsonBuilder *jb, const char *key) {
    jb_key(jb, key);
    jb_null(jb);
}

void jb_kv_raw(JsonBuilder *jb, const char *key, const char *raw_json) {
    jb_key(jb, key);
    jb_raw(jb, raw_json);
}
