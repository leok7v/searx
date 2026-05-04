#ifndef SEARX_C
#define SEARX_C

// searx.c: SearXNG meta-search client. Single translation unit.
//
//   cc searx.c -lcurl -o searx          builds the CLI binary.
//   #define SEARX_LIB before #include    drops main(), exposing
//                                        searx_run() to embed in your app.
//
// CLI flags:
//   --category <c>        searx category (default "general")
//   --time <t>            time range (day, week, month, year)
//   --timeout <s>         per-instance HTTP timeout in seconds (default 5)
//   --json                JSON output (default: human-readable)
//   --refresh-instances   force-refresh the searx.space cache
//   --saved-instances     only try previously-known reliable instances
//                         (auto-refreshes once if no saved list yet)
//   --verbose             full debug on stderr (otherwise non-json runs
//                         print minimal progress)
//   -h, --help            this help
//
// Tries JSON first; on a non-JSON 200 falls back to scraping the simple
// theme HTML on the same instance. Per-instance success counters and
// last-seen reachability live in one file and order subsequent runs.

#include "curly.c"
#include "json.c"

#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#  if defined(__has_include) && __has_include(<direct.h>)
#    include <direct.h>
#  endif
#  define searx_mkdir(p) _mkdir(p)
#else
#  define searx_mkdir(p) mkdir((p), 0700)
#endif

#define SEARX_REFRESH_URL    "https://searx.space/data/instances.json"
#define SEARX_DIR_NAME       ".searx"
#define SEARX_CACHE_BASE     "searx.space.data.instances"
#define SEARX_RELIABLE_BASE  "searx.space.data.instances.reliable"
#define SEARX_MAX_ATTEMPTS   20
#define SEARX_TIMEOUT_MS     5000
#define SEARX_REFRESH_MS     10000
#define SEARX_OFFLINE_TTL    3600
#define SEARX_RECENT_S       60
#define SEARX_DEADLINE_S     30

static const char * const SEARX_UAS[] = {
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) "
        "Gecko/20100101 Firefox/128.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:128.0) "
        "Gecko/20100101 Firefox/128.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/17.5 Safari/605.1.15",
};

#define SEARX_UAS_N (sizeof(SEARX_UAS) / sizeof(SEARX_UAS[0]))

struct searx_args {
    const char * category;
    const char * language;
    const char * time_range;
    struct chars query;
    int verbose;
    int human;
    int help;
    int refresh;
    int saved_only;
    int timeout_ms;
};

struct searx_inst {
    struct chars url;
    int json;
    int html;
    int state;            // 0 unknown, 1 online, 2 offline
    long checked_at;
};

struct searx_insts {
    struct searx_inst * data;
    size_t count;
    size_t capacity;
};

struct searx_ctx {
    struct chars dir;
    struct chars cache_path;
    struct chars reliable_path;
    struct chars winner;
    struct text fresh;
    struct text plan;
    struct searx_insts known;
};

static double searx_jnum(struct json * o, const char * k, double d) {
    struct json * j = json_get(o, k);
    return (j && j->kind == JSON_NUM) ? j->u.num : d;
}

static const char * searx_jstr(struct json * o, const char * k) {
    struct json * j = json_get(o, k);
    return (j && j->kind == JSON_STR) ? j->u.str.data : NULL;
}

static int searx_read_file(const char * p, struct chars * out) {
    int r = -1;
    FILE * f = fopen(p, "rb");
    if (f) {
        char buf[16384];
        size_t n = fread(buf, 1, sizeof(buf), f);
        while (n > 0) {
            chars_put(out, buf, n);
            n = fread(buf, 1, sizeof(buf), f);
        }
        fclose(f);
        r = 0;
    }
    return r;
}

static int searx_write_file(const char * p, const char * d, size_t n) {
    int r = -1;
    FILE * f = fopen(p, "wb");
    if (f) {
        if (fwrite(d, 1, n, f) == n) { r = 0; }
        fclose(f);
    }
    return r;
}

static int searx_isdir(const char * p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int searx_isfile(const char * p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0;
}

static const char * searx_tmpdir(void) {
    const char * t = getenv("TMPDIR");
    if (!t || !*t) { t = getenv("TMP"); }
    if (!t || !*t) { t = getenv("TEMP"); }
    if (!t || !*t) { t = "/tmp"; }
    return t;
}

static int searx_ask_yes(const char * q) {
    int yes = 0;
    if (isatty(fileno(stdin))) {
        fprintf(stderr, "%s [y/N] ", q);
        fflush(stderr);
        char b[16];
        if (fgets(b, sizeof(b), stdin) &&
            (b[0] == 'y' || b[0] == 'Y')) { yes = 1; }
    }
    return yes;
}

static void searx_path(struct chars * out, const char * d, const char * b) {
    chars_puts(out, d);
    if (out->count > 0 &&
        out->data[out->count - 1] != '/' &&
        out->data[out->count - 1] != '\\') {
        chars_put(out, "/", 1);
    }
    chars_puts(out, b);
}

static void searx_dir(struct chars * out) {
    int got = 0;
    const char * h = getenv("HOME");
    if (!h || !*h) { h = getenv("USERPROFILE"); }
    if (h && *h) {
        struct chars c = {0};
        chars_printf(&c, "%s/%s", h, SEARX_DIR_NAME);
        if (searx_isdir(c.data)) {
            chars_put(out, c.data, c.count);
            got = 1;
        } else {
            char p[512];
            snprintf(p, sizeof(p), "Create %s/?", c.data);
            if (searx_ask_yes(p) && searx_mkdir(c.data) == 0) {
                chars_put(out, c.data, c.count);
                got = 1;
            }
        }
        chars_free(&c);
    }
    if (!got) { chars_puts(out, searx_tmpdir()); }
}

static void searx_urlenc(struct chars * out, const char * s) {
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        int safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            char b = (char)c;
            chars_put(out, &b, 1);
        } else {
            chars_printf(out, "%%%02X", (unsigned)c);
        }
    }
}

static void searx_url(struct chars * out, const char * inst,
                      const struct searx_args * a, int as_json) {
    chars_puts(out, inst);
    chars_puts(out, "/search?q=");
    searx_urlenc(out, a->query.data ? a->query.data : "");
    if (as_json) { chars_puts(out, "&format=json"); }
    chars_puts(out, "&categories=");
    searx_urlenc(out, a->category);
    chars_puts(out, "&language=");
    searx_urlenc(out, a->language);
    if (a->time_range && a->time_range[0]) {
        chars_puts(out, "&time_range=");
        searx_urlenc(out, a->time_range);
    }
}

static int searx_http(const char * url, long ms,
                      struct chars * body, long * st) {
    struct curly c;
    int r = curly_init(&c);
    *st = 0;
    if (r == 0) {
        const char * ua = SEARX_UAS[(size_t)rand() % SEARX_UAS_N];
        curly_add_header(&c, "User-Agent", ua);
        curly_add_header(&c, "Accept",
            "text/html,application/xhtml+xml,application/xml;q=0.9,"
            "application/json;q=0.9,*/*;q=0.8");
        curly_add_header(&c, "Accept-Language", "en-US,en;q=0.9");
        curly_add_header(&c, "DNT", "1");
        curly_add_header(&c, "Upgrade-Insecure-Requests", "1");
        curly_add_header(&c, "Sec-Fetch-Dest", "document");
        curly_add_header(&c, "Sec-Fetch-Mode", "navigate");
        curly_add_header(&c, "Sec-Fetch-Site", "none");
        curly_add_header(&c, "Sec-Fetch-User", "?1");
        curly_set_timeout_ms(&c, ms);
        curl_easy_setopt(c.easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c.easy, CURLOPT_ACCEPT_ENCODING, "");
        int rc = curly_do(&c, url, "GET", NULL, 0);
        *st = c.status;
        if (c.body.data) { chars_put(body, c.body.data, c.body.count); }
        if (rc != 0 && *st == 0) { r = -1; }
    }
    curly_close(&c);
    return r;
}

static const char * searx_find(const char * a, const char * b,
                               const char * needle) {
    const char * r = NULL;
    if (a && b && a < b) {
        size_t hn = (size_t)(b - a);
        size_t nl = strlen(needle);
        if (hn >= nl) {
            size_t i = 0;
            while (!r && i + nl <= hn) {
                if (memcmp(a + i, needle, nl) == 0) { r = a + i; } else { i++; }
            }
        }
    }
    return r;
}

static int searx_contains(const struct chars * s, const char * n) {
    return searx_find(s->data, s->data + s->count, n) != NULL;
}

static const struct { const char * name; char ch; } searx_named[] = {
    {"quot", '"'}, {"apos", '\''}, {"amp", '&'},
    {"lt", '<'}, {"gt", '>'}, {"nbsp", ' '},
};

static int searx_named_decode(struct chars * out,
                              const char * name, size_t n) {
    int matched = 0;
    size_t cnt = sizeof(searx_named) / sizeof(searx_named[0]);
    for (size_t i = 0; !matched && i < cnt; i++) {
        const char * en = searx_named[i].name;
        if (strlen(en) == n && memcmp(en, name, n) == 0) {
            chars_put(out, &searx_named[i].ch, 1);
            matched = 1;
        }
    }
    return matched;
}

static uint32_t searx_num_decode(const char * s, const char * end) {
    uint32_t cp = 0;
    int hex = (s < end && (*s == 'x' || *s == 'X'));
    if (hex) { s++; }
    while (s < end) {
        char c = *s++;
        if (hex) {
            cp <<= 4;
            if (c >= '0' && c <= '9') {
                cp |= (uint32_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                cp |= (uint32_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                cp |= (uint32_t)(c - 'A' + 10);
            }
        } else if (c >= '0' && c <= '9') {
            cp = cp * 10 + (uint32_t)(c - '0');
        }
    }
    return cp;
}

static void searx_html_decode(struct chars * out,
                              const char * s, const char * end) {
    int in_tag = 0;
    int last_space = 1;
    while (s < end) {
        char c = *s;
        if (in_tag) {
            if (c == '>') { in_tag = 0; }
            s++;
        } else if (c == '<') {
            in_tag = 1;
            s++;
        } else if (c == '&') {
            const char * semi = NULL;
            const char * q = s + 1;
            while (!semi && q < end && q - s < 12) {
                if (*q == ';') { semi = q; } else { q++; }
            }
            if (!semi) {
                chars_put(out, &c, 1);
                last_space = 0;
                s++;
            } else {
                size_t n = (size_t)(semi - s - 1);
                if (n >= 2 && s[1] == '#') {
                    json_utf8(out, searx_num_decode(s + 2, semi));
                } else if (!searx_named_decode(out, s + 1, n)) {
                    chars_put(out, s, (size_t)(semi - s) + 1);
                }
                last_space = 0;
                s = semi + 1;
            }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space) {
                chars_put(out, " ", 1);
                last_space = 1;
            }
            s++;
        } else {
            chars_put(out, &c, 1);
            last_space = 0;
            s++;
        }
    }
    while (out->count > 0 &&
           (out->data[out->count - 1] == ' ' ||
            out->data[out->count - 1] == '\t')) {
        out->count--;
        out->data[out->count] = '\0';
    }
}

static void searx_push_result(struct json * results,
                              const char * us, const char * ue,
                              const char * to, const char * tc,
                              const char * cs, const char * ce) {
    struct chars t = {0}, c = {0};
    searx_html_decode(&t, to, tc);
    if (cs && ce && ce > cs) { searx_html_decode(&c, cs, ce); }
    struct json * it = json_new_obj();
    json_obj_put(it, "url", 3, json_new_str(us, (size_t)(ue - us)));
    json_obj_put(it, "title", 5, json_new_str(t.data, t.count));
    json_obj_put(it, "content", 7, json_new_str(c.data, c.count));
    json_arr_push(results, it);
    chars_free(&t);
    chars_free(&c);
}

static struct json * searx_scrape(const char * body, size_t len) {
    struct json * root = json_new_obj();
    struct json * results = json_new_arr();
    json_obj_put(root, "results", 7, results);
    const char * end = body + len;
    const char * p = body;
    while (p < end) {
        const char * art = searx_find(p, end, "<article class=\"result");
        const char * art_end = searx_find(art, end, "</article>");
        if (art && art_end) {
            const char * h3  = searx_find(art, art_end, "<h3>");
            const char * h3e = searx_find(h3, art_end, "</h3>");
            const char * a   = searx_find(h3, h3e, "<a ");
            const char * hr  = searx_find(a, h3e, "href=\"");
            const char * us  = hr ? hr + 6 : NULL;
            const char * ue  = us;
            while (ue && ue < h3e && *ue != '"') { ue++; }
            const char * to  = a;
            while (to && to < h3e && *to != '>') { to++; }
            if (to && to < h3e) { to++; }
            const char * tc  = searx_find(to, h3e, "</a>");
            const char * co  = searx_find(art, art_end,
                                          "<p class=\"content\">");
            const char * cs  = co ? co + 19 : NULL;
            const char * ce  = searx_find(cs, art_end, "</p>");
            if (us && ue > us && to && tc && tc > to) {
                searx_push_result(results, us, ue, to, tc, cs, ce);
            }
            p = art_end + 10;
        } else {
            p = end;
        }
    }
    return root;
}

static struct searx_inst * searx_inst_at(struct searx_insts * s,
                                         const char * url, int create) {
    size_t n = strlen(url);
    struct searx_inst * r = NULL;
    size_t i = 0;
    while (!r && i < s->count) {
        struct searx_inst * e = &s->data[i];
        if (e->url.count == n && memcmp(e->url.data, url, n) == 0) {
            r = e;
        } else {
            i++;
        }
    }
    if (!r && create) {
        arr_grow((struct arr *)s, sizeof(struct searx_inst),
                 s->count + 1);
        r = &s->data[s->count++];
        memset(r, 0, sizeof(*r));
        chars_put(&r->url, url, n);
    }
    return r;
}

static int searx_skip(struct searx_insts * s, const char * url, long now) {
    int skip = 0;
    struct searx_inst * e = searx_inst_at(s, url, 0);
    if (e) {
        long age = now - e->checked_at;
        if (e->state == 2 && age < SEARX_OFFLINE_TTL) { skip = 1; }
        if (e->state == 1 && age < SEARX_RECENT_S)    { skip = 1; }
    }
    return skip;
}

static void searx_insts_load(struct searx_insts * s, const char * p) {
    struct chars buf = {0};
    if (searx_read_file(p, &buf) == 0) {
        struct json * j = json_parse(buf.data, buf.count);
        struct json * arr = j ? json_get(j, "instances") : NULL;
        if (arr && arr->kind == JSON_ARR) {
            for (size_t i = 0; i < arr->u.arr.count; i++) {
                struct json * it = arr->u.arr.data[i];
                const char * url = searx_jstr(it, "url");
                if (url) {
                    struct searx_inst * e = searx_inst_at(s, url, 1);
                    e->json  = (int)searx_jnum(it, "json",  e->json);
                    e->html  = (int)searx_jnum(it, "html",  e->html);
                    e->state = (int)searx_jnum(it, "state", e->state);
                    e->checked_at =
                        (long)searx_jnum(it, "checked_at", e->checked_at);
                }
            }
        }
        json_free(j);
    }
    chars_free(&buf);
}

static int searx_inst_cmp(const void * a, const void * b) {
    const struct searx_inst * x = a;
    const struct searx_inst * y = b;
    int r = y->json - x->json;
    return r != 0 ? r : y->html - x->html;
}

static void searx_insts_save(struct searx_insts * s, const char * p) {
    qsort(s->data, s->count, sizeof(struct searx_inst), searx_inst_cmp);
    struct chars out = {0};
    chars_puts(&out, "{\"instances\":[\n");
    for (size_t i = 0; i < s->count; i++) {
        struct searx_inst * e = &s->data[i];
        if (i > 0) { chars_puts(&out, ",\n"); }
        chars_puts(&out, "  {\"url\":");
        struct json * t = json_new_str(e->url.data, e->url.count);
        json_write(t, &out);
        json_free(t);
        chars_printf(&out,
            ",\"json\":%d,\"html\":%d,"
            "\"state\":%d,\"checked_at\":%ld}",
            e->json, e->html, e->state, e->checked_at);
    }
    chars_puts(&out, "\n]}\n");
    searx_write_file(p, out.data, out.count);
    chars_free(&out);
}

static void searx_insts_free(struct searx_insts * s) {
    for (size_t i = 0; i < s->count; i++) { chars_free(&s->data[i].url); }
    free(s->data);
    s->data = NULL;
    s->count = 0;
    s->capacity = 0;
}

static void searx_ctx_free(struct searx_ctx * c) {
    chars_free(&c->dir);
    chars_free(&c->cache_path);
    chars_free(&c->reliable_path);
    chars_free(&c->winner);
    text_free(&c->fresh);
    text_free(&c->plan);
    searx_insts_free(&c->known);
}

static int searx_passes_filter(struct json * v, const struct chars * key) {
    int sc = (int)searx_jnum(json_get(v, "http"), "status_code", 0);
    struct json * tm = json_get(v, "timing");
    double sp = searx_jnum(json_get(tm, "search_go"),
                           "success_percentage", -1);
    if (sp < 0) {
        sp = searx_jnum(json_get(tm, "search"),
                        "success_percentage", -1);
    }
    int onion = 0;
    size_t k = 0;
    while (!onion && k + 6 <= key->count) {
        if (memcmp(key->data + k, ".onion", 6) == 0) {
            onion = 1;
        } else {
            k++;
        }
    }
    return !onion && sc == 200 && sp > 90.0 &&
           key->count > 8 &&
           memcmp(key->data, "https://", 8) == 0;
}

static void searx_collect(struct text * out, const struct chars * cache) {
    if (cache->count > 0) {
        struct json * j = json_parse(cache->data, cache->count);
        struct json * inst = j ? json_get(j, "instances") : NULL;
        if (inst && inst->kind == JSON_OBJ) {
            for (size_t i = 0; i < inst->u.obj.count; i++) {
                struct chars * key = inst->u.obj.data[i].key;
                struct json * v = inst->u.obj.data[i].val;
                if (searx_passes_filter(v, key)) {
                    size_t n = key->count;
                    while (n > 0 && key->data[n - 1] == '/') { n--; }
                    text_put(out, key->data, n);
                }
            }
        }
        json_free(j);
    }
}

static struct json * searx_usable(struct chars * raw, int as_json) {
    struct json * j = NULL;
    if (as_json) {
        size_t i = 0;
        while (i < raw->count &&
               (raw->data[i] == ' ' || raw->data[i] == '\t' ||
                raw->data[i] == '\r' || raw->data[i] == '\n')) {
            i++;
        }
        if (i < raw->count &&
            (raw->data[i] == '{' || raw->data[i] == '[')) {
            j = json_parse(raw->data, raw->count);
        }
    } else if (searx_contains(raw, "<article class=\"result")) {
        j = searx_scrape(raw->data, raw->count);
    }
    if (j) {
        struct json * r = json_get(j, "results");
        int n = (r && r->kind == JSON_ARR) ? (int)r->u.arr.count : 0;
        if (n == 0) { json_free(j); j = NULL; }
    }
    return j;
}

static int searx_attempt(const char * inst, const struct searx_args * a,
                         int * via_json, int * reachable,
                         struct chars * raw, struct json ** parsed) {
    int ok = 0;
    int as_json = 1;
    int done = 0;
    *via_json = 0;
    *reachable = 0;
    while (!done) {
        struct chars url = {0};
        searx_url(&url, inst, a, as_json);
        chars_free(raw);
        long st = 0;
        int rc = searx_http(url.data, a->timeout_ms, raw, &st);
        chars_free(&url);
        if (rc == 0) { *reachable = 1; }
        if (rc == 0 && st == 200) {
            struct json * j = searx_usable(raw, as_json);
            if (j) {
                *parsed = j;
                *via_json = as_json;
                ok = 1;
            }
        }
        if (ok || !as_json) { done = 1; } else { as_json = 0; }
    }
    return ok;
}

static int searx_refresh(const char * cache_path, int show) {
    int r = -1;
    if (show) { fprintf(stderr, "Refreshing instance cache...\n"); }
    struct chars body = {0};
    long st = 0;
    if (searx_http(SEARX_REFRESH_URL, SEARX_REFRESH_MS, &body, &st) == 0 &&
        st == 200 && body.count > 0) {
        struct json * j = json_parse(body.data, body.count);
        if (j && j->kind == JSON_OBJ && json_get(j, "instances") &&
            searx_write_file(cache_path, body.data, body.count) == 0) {
            r = 0;
        }
        json_free(j);
    }
    chars_free(&body);
    if (r != 0 && show) { fprintf(stderr, "  refresh failed\n"); }
    return r;
}

static void searx_shuffle(struct text * t) {
    if (t->count > 1) {
        for (size_t i = t->count - 1; i > 0; i--) {
            size_t k = (size_t)rand() % (i + 1);
            char * tmp = t->data[i];
            t->data[i] = t->data[k];
            t->data[k] = tmp;
        }
    }
}

static void searx_plan(struct text * plan, struct searx_insts * known,
                       const struct text * fresh, long now,
                       int saved_only) {
    struct text picked = {0};
    for (size_t i = 0; i < known->count; i++) {
        struct searx_inst * e = &known->data[i];
        if (!searx_skip(known, e->url.data, now)) {
            text_put(&picked, e->url.data, e->url.count);
        }
    }
    searx_shuffle(&picked);
    for (size_t i = 0; i < picked.count; i++) {
        text_puts(plan, picked.data[i]);
    }
    text_free(&picked);
    if (!saved_only) {
        for (size_t i = 0; i < fresh->count; i++) {
            const char * u = fresh->data[i];
            if (!searx_inst_at(known, u, 0) &&
                !searx_skip(known, u, now)) {
                text_puts(plan, u);
            }
        }
    }
}

static int searx_search(const struct searx_args * a,
                        const struct text * plan,
                        struct searx_insts * known,
                        long deadline,
                        struct json ** found,
                        int * via_json,
                        struct chars * winner) {
    struct chars raw = {0};
    int got = 0;
    int show = a->verbose || a->human;
    size_t lim = plan->count;
    if (lim > SEARX_MAX_ATTEMPTS) { lim = SEARX_MAX_ATTEMPTS; }
    for (size_t i = 0; !got && i < lim && time(NULL) < deadline; i++) {
        const char * inst = plan->data[i];
        long now = time(NULL);
        if (show) {
            fprintf(stderr, "Attempt %zu/%zu: %s\n", i + 1, lim, inst);
        }
        int reachable = 0;
        struct json * j = NULL;
        struct searx_inst * e = searx_inst_at(known, inst, 1);
        e->checked_at = now;
        if (searx_attempt(inst, a, via_json, &reachable, &raw, &j)) {
            *found = j;
            chars_puts(winner, inst);
            if (*via_json) { e->json++; } else { e->html++; }
            e->state = 1;
            got = 1;
            if (show) {
                fprintf(stderr, "  ok via %s\n", *via_json ? "json" : "html");
            }
        } else {
            e->state = reachable ? 1 : 2;
            const char * why = reachable ? "no results / blocked"
                                         : "unreachable";
            if (show) { fprintf(stderr, "  %s\n", why); }
        }
    }
    if (!got && show && time(NULL) >= deadline) {
        fprintf(stderr, "Time budget exhausted.\n");
    }
    chars_free(&raw);
    return got;
}

static void searx_ctx_reload_fresh(struct searx_ctx * c, int saved_only) {
    text_free(&c->fresh);
    if (!saved_only) {
        struct chars cdata = {0};
        searx_read_file(c->cache_path.data, &cdata);
        searx_collect(&c->fresh, &cdata);
        searx_shuffle(&c->fresh);
        chars_free(&cdata);
    }
}

static void searx_ctx_replan(struct searx_ctx * c, long now,
                             int saved_only) {
    text_free(&c->plan);
    searx_plan(&c->plan, &c->known, &c->fresh, now, saved_only);
}

static void searx_emit_human(FILE * f, struct json * root) {
    struct json * results = json_get(root, "results");
    if (results && results->kind == JSON_ARR) {
        for (size_t i = 0; i < results->u.arr.count; i++) {
            struct json * it = results->u.arr.data[i];
            const char * t = searx_jstr(it, "title");
            const char * u = searx_jstr(it, "url");
            const char * c = searx_jstr(it, "content");
            fprintf(f, "Title: %s\nURL:   %s\nSnippet: %s\n\n",
                    t ? t : "", u ? u : "", c ? c : "");
        }
    }
}

static void searx_emit(FILE * f, struct json * found, int human) {
    if (human) {
        searx_emit_human(f, found);
    } else {
        struct chars out = {0};
        json_write(found, &out);
        chars_put(&out, "\n", 1);
        fwrite(out.data, 1, out.count, f);
        chars_free(&out);
    }
}

static void searx_help(FILE * f) {
    fprintf(f,
        "Usage: searx [options] [--] <query>\n"
        "  --category <c>        category, default \"general\"\n"
        "  --time <t>            time range (day, week, month, year)\n"
        "  --timeout <s>         per-instance HTTP timeout (default 5)\n"
        "  --json                JSON output (default: human-readable)\n"
        "  --refresh-instances   force-fetch the searx.space cache\n"
        "  --saved-instances     only try previously-known instances\n"
        "  --verbose             full debug on stderr\n"
        "  -h, --help            this help\n");
}

static int searx_args(struct searx_args * a, int argc, char ** argv) {
    a->category = "general";
    a->language = "en-US";
    a->time_range = "";
    a->human = 1;
    a->timeout_ms = SEARX_TIMEOUT_MS;
    int r = 0;
    int i = 1;
    int qstart = -1;
    while (r == 0 && qstart < 0 && i < argc) {
        const char * s = argv[i];
        if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
            a->help = 1;
            i++;
        } else if (strcmp(s, "--verbose") == 0) {
            a->verbose = 1;
            i++;
        } else if (strcmp(s, "--json") == 0) {
            a->human = 0;
            i++;
        } else if (strcmp(s, "--refresh-instances") == 0) {
            a->refresh = 1;
            i++;
        } else if (strcmp(s, "--saved-instances") == 0) {
            a->saved_only = 1;
            i++;
        } else if (strcmp(s, "--category") == 0 && i + 1 < argc) {
            a->category = argv[i + 1];
            i += 2;
        } else if (strcmp(s, "--time") == 0 && i + 1 < argc) {
            a->time_range = argv[i + 1];
            i += 2;
        } else if (strcmp(s, "--timeout") == 0 && i + 1 < argc) {
            int t = atoi(argv[i + 1]);
            if (t < 1)   { t = 1; }
            if (t > 300) { t = 300; }
            a->timeout_ms = t * 1000;
            i += 2;
        } else if (strcmp(s, "--") == 0) {
            qstart = i + 1;
        } else if (s[0] == '-' && s[1] != '\0') {
            r = -1;
        } else {
            qstart = i;
        }
    }
    if (r == 0 && qstart >= 0) {
        for (int j = qstart; j < argc; j++) {
            if (j > qstart) { chars_put(&a->query, " ", 1); }
            chars_puts(&a->query, argv[j]);
        }
    }
    return r;
}

__attribute__((unused))
static inline int searx_run(int argc, char ** argv, char ** env) {
    (void)env;
    struct searx_args a = {0};
    int rc = 0;
    if (searx_args(&a, argc, argv) != 0) {
        searx_help(stderr);
        rc = 2;
    }
    if (rc == 0 && a.help) { searx_help(stdout); }
    if (rc == 0 && !a.help && a.query.count == 0) {
        searx_help(stderr);
        rc = 2;
    }
    if (rc == 0 && !a.help && a.query.count > 0) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        int show = a.verbose || a.human;
        struct searx_ctx c = {0};
        searx_dir(&c.dir);
        searx_path(&c.cache_path, c.dir.data, SEARX_CACHE_BASE);
        searx_path(&c.reliable_path, c.dir.data, SEARX_RELIABLE_BASE);
        if (a.verbose) { fprintf(stderr, "Cache dir: %s\n", c.dir.data); }
        int refreshed = 0;
        int saved_only = a.saved_only;
        if (a.refresh) {
            searx_refresh(c.cache_path.data, show);
            refreshed = 1;
        }
        if (saved_only && !searx_isfile(c.reliable_path.data)) {
            if (show) {
                fprintf(stderr,
                    "No saved instances yet; discovering this run.\n");
            }
            if (!refreshed) {
                searx_refresh(c.cache_path.data, show);
                refreshed = 1;
            }
            saved_only = 0;
        }
        searx_insts_load(&c.known, c.reliable_path.data);
        long now = time(NULL);
        long deadline_s = (long)a.timeout_ms / 1000 * 6;
        if (deadline_s < SEARX_DEADLINE_S) { deadline_s = SEARX_DEADLINE_S; }
        long deadline = now + deadline_s;
        searx_ctx_reload_fresh(&c, saved_only);
        searx_ctx_replan(&c, now, saved_only);
        if (a.verbose) {
            fprintf(stderr, "Plan: %zu (%zu known, %zu fresh%s)\n",
                    c.plan.count, c.known.count, c.fresh.count,
                    saved_only ? ", saved-only" : "");
        }
        struct json * found = NULL;
        int via = 0;
        searx_search(&a, &c.plan, &c.known, deadline,
                     &found, &via, &c.winner);
        if (!found && !saved_only && !refreshed &&
            time(NULL) < deadline &&
            searx_refresh(c.cache_path.data, show) == 0) {
            refreshed = 1;
            searx_ctx_reload_fresh(&c, 0);
            searx_ctx_replan(&c, now, 0);
            if (a.verbose) {
                fprintf(stderr, "Plan: %zu instance(s)\n", c.plan.count);
            }
            searx_search(&a, &c.plan, &c.known, deadline,
                         &found, &via, &c.winner);
        }
        searx_insts_save(&c.known, c.reliable_path.data);
        if (found) {
            searx_emit(stdout, found, a.human);
            json_free(found);
        } else {
            if (show) { fprintf(stderr, "No results.\n"); }
            rc = 1;
        }
        searx_ctx_free(&c);
    }
    chars_free(&a.query);
    return rc;
}

#ifndef SEARX_LIB
int main(int argc, char * argv[], char * env[]) {
    return searx_run(argc, argv, env);
}
#endif

#endif /* SEARX_C */
