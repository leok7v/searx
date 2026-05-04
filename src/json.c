#ifndef JSON_C
#define JSON_C

#include "chars.c"

enum json_kind {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
};

struct json;

struct json_pair {
    struct chars * key;
    struct json * val;
};

struct json {
    enum json_kind kind;
    union {
        int b;
        double num;
        struct chars str;
        struct {
            struct json ** data;
            size_t count;
            size_t capacity;
        } arr;
        struct {
            struct json_pair * data;
            size_t count;
            size_t capacity;
        } obj;
    } u;
};

struct json_parser {
    const char * s;
    const char * end;
    int error;
};

static inline struct json * json_new(enum json_kind k) {
    struct json * j = oom(calloc(1, sizeof(*j)));
    j->kind = k;
    return j;
}

static inline struct json * json_new_null(void) {
    return json_new(JSON_NULL);
}

static inline struct json * json_new_bool(int b) {
    struct json * j = json_new(JSON_BOOL);
    j->u.b = b ? 1 : 0;
    return j;
}

static inline struct json * json_new_num(double v) {
    struct json * j = json_new(JSON_NUM);
    j->u.num = v;
    return j;
}

static inline struct json * json_new_str(const char * s, size_t n) {
    struct json * j = json_new(JSON_STR);
    chars_put(&j->u.str, s, n);
    return j;
}

static inline struct json * json_new_arr(void) {
    return json_new(JSON_ARR);
}

static inline struct json * json_new_obj(void) {
    return json_new(JSON_OBJ);
}

static inline void json_free(struct json * j);

static inline void json_arr_push(struct json * arr, struct json * item) {
    arr_grow((struct arr *)&arr->u.arr, sizeof(struct json *),
             arr->u.arr.count + 1);
    arr->u.arr.data[arr->u.arr.count++] = item;
}

static inline void json_obj_put(struct json * obj, const char * k, size_t kn,
                         struct json * val) {
    struct chars * key = oom(calloc(1, sizeof(*key)));
    chars_put(key, k, kn);
    arr_grow((struct arr *)&obj->u.obj, sizeof(struct json_pair),
             obj->u.obj.count + 1);
    obj->u.obj.data[obj->u.obj.count].key = key;
    obj->u.obj.data[obj->u.obj.count].val = val;
    obj->u.obj.count++;
}

static inline void json_free(struct json * j) {
    if (j) {
        if (j->kind == JSON_STR) {
            chars_free(&j->u.str);
        } else if (j->kind == JSON_ARR) {
            for (size_t i = 0; i < j->u.arr.count; i++) {
                json_free(j->u.arr.data[i]);
            }
            free(j->u.arr.data);
        } else if (j->kind == JSON_OBJ) {
            for (size_t i = 0; i < j->u.obj.count; i++) {
                struct chars * k = j->u.obj.data[i].key;
                chars_free(k);
                free(k);
                json_free(j->u.obj.data[i].val);
            }
            free(j->u.obj.data);
        }
        free(j);
    }
}

static inline struct json * json_get(struct json * obj, const char * k) {
    struct json * r = NULL;
    if (obj && obj->kind == JSON_OBJ) {
        size_t kn = strlen(k);
        int done = 0;
        for (size_t i = 0; !done && i < obj->u.obj.count; i++) {
            struct chars * ki = obj->u.obj.data[i].key;
            if (ki->count == kn && memcmp(ki->data, k, kn) == 0) {
                r = obj->u.obj.data[i].val;
                done = 1;
            }
        }
    }
    return r;
}

static inline void json_utf8(struct chars * out, uint32_t cp) {
    if (cp < 0x80) {
        char b = (char)cp;
        chars_put(out, &b, 1);
    } else if (cp < 0x800) {
        char b[2];
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        chars_put(out, b, 2);
    } else if (cp < 0x10000) {
        char b[3];
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        chars_put(out, b, 3);
    } else {
        char b[4];
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        chars_put(out, b, 4);
    }
}

static inline void json_write_str(const struct chars * s, struct chars * out) {
    chars_put(out, "\"", 1);
    for (size_t i = 0; i < s->count; i++) {
        char c = s->data[i];
        if (c == '"') {
            chars_put(out, "\\\"", 2);
        } else if (c == '\\') {
            chars_put(out, "\\\\", 2);
        } else if (c == '\n') {
            chars_put(out, "\\n", 2);
        } else if (c == '\r') {
            chars_put(out, "\\r", 2);
        } else if (c == '\t') {
            chars_put(out, "\\t", 2);
        } else if (c == '\b') {
            chars_put(out, "\\b", 2);
        } else if (c == '\f') {
            chars_put(out, "\\f", 2);
        } else if ((unsigned char)c < 0x20) {
            chars_printf(out, "\\u%04x", (unsigned)(unsigned char)c);
        } else {
            chars_put(out, &c, 1);
        }
    }
    chars_put(out, "\"", 1);
}

static inline void json_write(struct json * j, struct chars * out) {
    if (j == NULL || j->kind == JSON_NULL) {
        chars_put(out, "null", 4);
    } else if (j->kind == JSON_BOOL) {
        if (j->u.b) {
            chars_put(out, "true", 4);
        } else {
            chars_put(out, "false", 5);
        }
    } else if (j->kind == JSON_NUM) {
        chars_printf(out, "%.17g", j->u.num);
    } else if (j->kind == JSON_STR) {
        json_write_str(&j->u.str, out);
    } else if (j->kind == JSON_ARR) {
        chars_put(out, "[", 1);
        for (size_t i = 0; i < j->u.arr.count; i++) {
            if (i > 0) { chars_put(out, ",", 1); }
            json_write(j->u.arr.data[i], out);
        }
        chars_put(out, "]", 1);
    } else if (j->kind == JSON_OBJ) {
        chars_put(out, "{", 1);
        for (size_t i = 0; i < j->u.obj.count; i++) {
            if (i > 0) { chars_put(out, ",", 1); }
            json_write_str(j->u.obj.data[i].key, out);
            chars_put(out, ":", 1);
            json_write(j->u.obj.data[i].val, out);
        }
        chars_put(out, "}", 1);
    }
}

static inline void json_skip_ws(struct json_parser * p) {
    int done = 0;
    while (!done && p->s < p->end) {
        char c = *p->s;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->s++;
        } else {
            done = 1;
        }
    }
}

static inline int json_hex4(const char * s, uint32_t * out) {
    int ok = 1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char h = s[i];
        v <<= 4;
        if (h >= '0' && h <= '9') {
            v |= (uint32_t)(h - '0');
        } else if (h >= 'a' && h <= 'f') {
            v |= (uint32_t)(h - 'a' + 10);
        } else if (h >= 'A' && h <= 'F') {
            v |= (uint32_t)(h - 'A' + 10);
        } else {
            ok = 0;
        }
    }
    *out = v;
    return ok;
}

static inline void json_parse_str(struct json_parser * p, struct chars * out) {
    chars_put(out, "", 0);
    if (p->s < p->end && *p->s == '"') {
        p->s++;
        int done = 0;
        while (!done && !p->error && p->s < p->end) {
            char c = *p->s;
            if (c == '"') {
                p->s++;
                done = 1;
            } else if (c == '\\') {
                p->s++;
                if (p->s < p->end) {
                    char e = *p->s;
                    p->s++;
                    if (e == '"') { chars_put(out, "\"", 1); }
                    else if (e == '\\') { chars_put(out, "\\", 1); }
                    else if (e == '/') { chars_put(out, "/", 1); }
                    else if (e == 'b') { chars_put(out, "\b", 1); }
                    else if (e == 'f') { chars_put(out, "\f", 1); }
                    else if (e == 'n') { chars_put(out, "\n", 1); }
                    else if (e == 'r') { chars_put(out, "\r", 1); }
                    else if (e == 't') { chars_put(out, "\t", 1); }
                    else if (e == 'u') {
                        if (p->s + 4 <= p->end) {
                            uint32_t cp = 0;
                            if (json_hex4(p->s, &cp)) {
                                p->s += 4;
                                json_utf8(out, cp);
                            } else {
                                p->error = 1;
                            }
                        } else {
                            p->error = 1;
                        }
                    } else {
                        p->error = 1;
                    }
                } else {
                    p->error = 1;
                }
            } else {
                chars_put(out, &c, 1);
                p->s++;
            }
        }
        if (!done && !p->error) { p->error = 1; }
    } else {
        p->error = 1;
    }
}

static inline double json_parse_num(struct json_parser * p) {
    char buf[64];
    size_t n = 0;
    int done = 0;
    while (!done && p->s < p->end && n + 1 < sizeof(buf)) {
        char c = *p->s;
        int ok = (c >= '0' && c <= '9') || c == '-' || c == '+'
              || c == '.' || c == 'e' || c == 'E';
        if (ok) {
            buf[n++] = c;
            p->s++;
        } else {
            done = 1;
        }
    }
    buf[n] = '\0';
    double v = 0;
    if (n == 0) {
        p->error = 1;
    } else {
        char * ep = NULL;
        v = strtod(buf, &ep);
        if (ep == buf) { p->error = 1; }
    }
    return v;
}

static inline int json_literal(struct json_parser * p, const char * lit) {
    size_t ln = strlen(lit);
    int ok = 0;
    if ((size_t)(p->end - p->s) >= ln
        && memcmp(p->s, lit, ln) == 0) {
        p->s += ln;
        ok = 1;
    } else {
        p->error = 1;
    }
    return ok;
}

static inline struct json * json_parse_value(struct json_parser * p);

static inline struct json * json_parse_arr(struct json_parser * p) {
    struct json * n = json_new(JSON_ARR);
    p->s++;
    json_skip_ws(p);
    if (p->s < p->end && *p->s == ']') {
        p->s++;
    } else {
        int done = 0;
        while (!done && !p->error) {
            struct json * item = json_parse_value(p);
            if (!p->error) {
                json_arr_push(n, item);
                json_skip_ws(p);
                if (p->s < p->end && *p->s == ',') {
                    p->s++;
                    json_skip_ws(p);
                } else if (p->s < p->end && *p->s == ']') {
                    p->s++;
                    done = 1;
                } else {
                    p->error = 1;
                }
            } else {
                json_free(item);
            }
        }
    }
    return n;
}

static inline struct json * json_parse_obj(struct json_parser * p) {
    struct json * n = json_new(JSON_OBJ);
    p->s++;
    json_skip_ws(p);
    if (p->s < p->end && *p->s == '}') {
        p->s++;
    } else {
        int done = 0;
        while (!done && !p->error) {
            json_skip_ws(p);
            struct chars * key = oom(calloc(1, sizeof(*key)));
            json_parse_str(p, key);
            if (!p->error) {
                json_skip_ws(p);
                if (p->s < p->end && *p->s == ':') {
                    p->s++;
                    json_skip_ws(p);
                    struct json * val = json_parse_value(p);
                    if (!p->error) {
                        arr_grow((struct arr *)&n->u.obj,
                                 sizeof(struct json_pair),
                                 n->u.obj.count + 1);
                        n->u.obj.data[n->u.obj.count].key = key;
                        n->u.obj.data[n->u.obj.count].val = val;
                        n->u.obj.count++;
                        json_skip_ws(p);
                        if (p->s < p->end && *p->s == ',') {
                            p->s++;
                        } else if (p->s < p->end && *p->s == '}') {
                            p->s++;
                            done = 1;
                        } else {
                            p->error = 1;
                        }
                    } else {
                        chars_free(key);
                        free(key);
                        json_free(val);
                    }
                } else {
                    chars_free(key);
                    free(key);
                    p->error = 1;
                }
            } else {
                chars_free(key);
                free(key);
            }
        }
    }
    return n;
}

static inline struct json * json_parse_value(struct json_parser * p) {
    struct json * r = NULL;
    json_skip_ws(p);
    if (!p->error && p->s < p->end) {
        char c = *p->s;
        if (c == '{') {
            r = json_parse_obj(p);
        } else if (c == '[') {
            r = json_parse_arr(p);
        } else if (c == '"') {
            r = json_new(JSON_STR);
            json_parse_str(p, &r->u.str);
        } else if (c == 't') {
            if (json_literal(p, "true")) {
                r = json_new_bool(1);
            }
        } else if (c == 'f') {
            if (json_literal(p, "false")) {
                r = json_new_bool(0);
            }
        } else if (c == 'n') {
            if (json_literal(p, "null")) {
                r = json_new_null();
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            double v = json_parse_num(p);
            if (!p->error) {
                r = json_new_num(v);
            }
        } else {
            p->error = 1;
        }
    } else {
        p->error = 1;
    }
    return r;
}

static inline struct json * json_parse(const char * src, size_t n) {
    struct json_parser p = { src, src + n, 0 };
    struct json * r = json_parse_value(&p);
    json_skip_ws(&p);
    if (!p.error && p.s < p.end) {
        p.error = 1;
    }
    if (p.error) {
        json_free(r);
        r = NULL;
    }
    return r;
}

static inline struct json * json_parsel(const char * src, size_t n,
                                 size_t * consumed) {
    struct json_parser p = { src, src + n, 0 };
    struct json * r = json_parse_value(&p);
    if (!p.error) {
        while (p.s < p.end
               && (*p.s == ' ' || *p.s == '\t' || *p.s == '\r')) {
            p.s++;
        }
        if (p.s < p.end && *p.s == '\n') {
            p.s++;
        }
    }
    if (p.error) {
        json_free(r);
        r = NULL;
        *consumed = 0;
    } else {
        *consumed = (size_t)(p.s - src);
    }
    return r;
}

#ifdef JSON_TESTS

static void test_json_basic(void) {
    struct json * n = json_parse("null", 4);
    assert(n && n->kind == JSON_NULL);
    json_free(n);
    n = json_parse("true", 4);
    assert(n && n->kind == JSON_BOOL && n->u.b == 1);
    json_free(n);
    n = json_parse("false", 5);
    assert(n && n->kind == JSON_BOOL && n->u.b == 0);
    json_free(n);
    n = json_parse("42", 2);
    assert(n && n->kind == JSON_NUM && n->u.num == 42.0);
    json_free(n);
    n = json_parse("-3.5", 4);
    assert(n && n->kind == JSON_NUM && n->u.num == -3.5);
    json_free(n);
    n = json_parse("1e2", 3);
    assert(n && n->u.num == 100.0);
    json_free(n);
}

static void test_json_string(void) {
    struct json * n = json_parse("\"hello\"", 7);
    assert(n && n->kind == JSON_STR);
    assert(n->u.str.count == 5);
    assert(strcmp(n->u.str.data, "hello") == 0);
    json_free(n);
    n = json_parse("\"a\\nb\\\"c\"", 9);
    assert(n);
    assert(n->u.str.count == 5);
    assert(strcmp(n->u.str.data, "a\nb\"c") == 0);
    json_free(n);
    n = json_parse("\"\"", 2);
    assert(n && n->kind == JSON_STR);
    assert(n->u.str.data != NULL);
    assert(n->u.str.count == 0);
    json_free(n);
    n = json_parse("\"\\u0041\"", 8);
    assert(n && strcmp(n->u.str.data, "A") == 0);
    json_free(n);
}

static void test_json_array(void) {
    struct json * n = json_parse("[1,2,3]", 7);
    assert(n && n->kind == JSON_ARR);
    assert(n->u.arr.count == 3);
    assert(n->u.arr.data[0]->u.num == 1.0);
    assert(n->u.arr.data[2]->u.num == 3.0);
    json_free(n);
    n = json_parse("[]", 2);
    assert(n && n->kind == JSON_ARR && n->u.arr.count == 0);
    json_free(n);
    const char * s = "[true, \"x\", null]";
    n = json_parse(s, strlen(s));
    assert(n && n->u.arr.count == 3);
    assert(n->u.arr.data[0]->kind == JSON_BOOL);
    assert(n->u.arr.data[1]->kind == JSON_STR);
    assert(n->u.arr.data[2]->kind == JSON_NULL);
    json_free(n);
}

static void test_json_object(void) {
    const char * src = "{\"a\":1,\"b\":\"hi\",\"c\":[1,2]}";
    struct json * n = json_parse(src, strlen(src));
    assert(n && n->kind == JSON_OBJ);
    assert(n->u.obj.count == 3);
    assert(json_get(n, "a")->u.num == 1.0);
    assert(strcmp(json_get(n, "b")->u.str.data, "hi") == 0);
    struct json * c = json_get(n, "c");
    assert(c && c->kind == JSON_ARR && c->u.arr.count == 2);
    assert(json_get(n, "missing") == NULL);
    json_free(n);
    n = json_parse("{}", 2);
    assert(n && n->kind == JSON_OBJ && n->u.obj.count == 0);
    json_free(n);
}

static void test_json_roundtrip(void) {
    const char * src = "{\"k\":[1,2,3],\"s\":\"he\\nllo\"}";
    struct json * n = json_parse(src, strlen(src));
    assert(n);
    struct chars out = {0};
    json_write(n, &out);
    assert(out.data != NULL);
    struct json * m = json_parse(out.data, out.count);
    assert(m);
    assert(json_get(m, "k")->u.arr.count == 3);
    assert(strcmp(json_get(m, "s")->u.str.data, "he\nllo") == 0);
    chars_free(&out);
    json_free(n);
    json_free(m);
}

static void test_json_build(void) {
    struct json * obj = json_new_obj();
    json_obj_put(obj, "x", 1, json_new_num(42));
    json_obj_put(obj, "name", 4, json_new_str("foo", 3));
    struct json * arr = json_new_arr();
    json_arr_push(arr, json_new_bool(1));
    json_arr_push(arr, json_new_null());
    json_obj_put(obj, "list", 4, arr);
    struct chars out = {0};
    json_write(obj, &out);
    struct json * back = json_parse(out.data, out.count);
    assert(back);
    assert(json_get(back, "x")->u.num == 42.0);
    assert(strcmp(json_get(back, "name")->u.str.data, "foo") == 0);
    assert(json_get(back, "list")->u.arr.data[0]->u.b == 1);
    assert(json_get(back, "list")->u.arr.data[1]->kind == JSON_NULL);
    chars_free(&out);
    json_free(obj);
    json_free(back);
}

static void test_json_error(void) {
    assert(json_parse("", 0) == NULL);
    assert(json_parse("{", 1) == NULL);
    assert(json_parse("[1,", 3) == NULL);
    assert(json_parse("{\"a\":}", 6) == NULL);
    assert(json_parse("truee", 5) == NULL);
    assert(json_parse("xyz", 3) == NULL);
}

static void test_json_jsonl(void) {
    const char * src = "{\"a\":1}\n{\"b\":2}\n";
    size_t n = strlen(src);
    size_t used = 0;
    struct json * j1 = json_parsel(src, n, &used);
    assert(j1 && used > 0);
    assert(json_get(j1, "a")->u.num == 1.0);
    size_t used2 = 0;
    struct json * j2 = json_parsel(src + used, n - used, &used2);
    assert(j2);
    assert(json_get(j2, "b")->u.num == 2.0);
    json_free(j1);
    json_free(j2);
}

static void test(void) {
    test_json_basic();
    test_json_string();
    test_json_array();
    test_json_object();
    test_json_roundtrip();
    test_json_build();
    test_json_error();
    test_json_jsonl();
}

int main(int argc, char * argv[], char * env[]) {
    (void)argc; (void)argv; (void)env;
    test();
    printf("OK\n");
    return 0;
}

#endif /* JSON_TESTS */

#endif /* JSON_C */
