#ifndef SEARX_C
#define SEARX_C

// searx.c: SearXNG meta-search client. Single translation unit.
//
//   cc searx.c -lcurl -o searx          builds the CLI binary.
//   #define SEARX_LIB before #include    drops main(), exposing
//                                        searx_run() and searx_query()
//                                        to embed in your app.
//
// CLI flags:
//   --category <c>        searx category (default "general")
//   --time <t>            time range (day, week, month, year)
//   --timeout <s>         per-instance HTTP timeout in seconds (default 5)
//   --json                JSON output (default: human-readable)
//   --refresh-instances   force-refresh the searx.space cache
//   --saved-instances     only try previously-known reliable instances
//                         (auto-refreshes once if no saved list yet)
//   --verbose             full debug on stderr
//   --no-console-io       suppress all stderr output (scripted use)
//   --test                run searx_test() smoke test and exit
//   -h, --help            this help
//
// Library API (when compiled with SEARX_LIB defined):
//   int  searx_run  (int argc, char ** argv, char ** env);
//   bool searx_query(const char * query, struct chars * out, bool json);
//
// Each attempt sends a single GET — no JSON-then-HTML double probe on the
// same host (that pattern reliably gets the client banned). Default human
// mode requests the simple HTML theme; --json requests format=json. If a
// host's history (e->json / e->html counters) says it only serves the
// other protocol, we use that instead.
//
// All persistent state lives in one file: ~/.searx/searx.json
//   { "fetched_at": <utc>,
//     "instances": [
//       {"url":..., "json":N, "html":M,
//        "checked_at":<utc>, "last_win_at":<utc>}, ...
//     ] }
// Two cooldowns, both load-bearing:
//   * SEARX_LOSE_TTL (60 s) — uniform anti-hammer floor; every visit
//     bumps checked_at, so no host gets re-hit within a minute, ever,
//     in any run, even within the same run.
//   * SEARX_WIN_TTL (600 s) — extra "be polite" delay on hosts that
//     just successfully served us, so we rotate across the pool
//     instead of hammering the same winner.
// Counters (json / html) rank hosts on cold-plan: proven hosts go
// before unknowns, with shuffling within each tier to spread load.

#include "curly.c"
#include "json.c"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
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
#define SEARX_DATA_BASE      "searx.json"
#define SEARX_MAX_ATTEMPTS   20
#define SEARX_TIMEOUT_MS     5000
#define SEARX_REFRESH_MS     10000
#define SEARX_LOSE_TTL       60    // anti-hammer floor for every visit;
                                   // never re-hit a host within 60 s,
                                   // win or lose, in any run.
#define SEARX_WIN_TTL        600   // extra cooldown on hosts that just
                                   // served us, so we rotate the pool.
#define SEARX_DEADLINE_S     30

// Console output modes (struct searx_args::show):
//   SILENT   no stderr output (--no-console-io, or --json without verbose)
//   BRIEF    one line per attempt (default human, when stderr is not tty)
//   PROGRESS single-line \r-overwritten status (default human + tty stderr)
//   VERBOSE  full multi-line debug (--verbose)
enum {
    SEARX_SHOW_SILENT,
    SEARX_SHOW_BRIEF,
    SEARX_SHOW_PROGRESS,
    SEARX_SHOW_VERBOSE,
};

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
    bool verbose;
    bool human;
    bool help;
    bool refresh;
    bool saved_only;
    bool no_console_io;
    bool test;
    int  timeout_ms;
    int  show;            // SEARX_SHOW_* — derived from flags + tty
};

struct searx_inst {
    struct chars url;
    int  json;            // count of past successful JSON responses
    int  html;            // count of past successful HTML scrapes
    long checked_at;      // UTC of last attempt (any outcome) — drives
                          // the LOSE_TTL anti-hammer floor
    long last_win_at;     // UTC of last successful response — drives
                          // the WIN_TTL "be polite to winners" cooldown
};

struct searx_insts {
    struct searx_inst * data;
    size_t count;
    size_t capacity;
};

struct searx_ctx {
    struct chars dir;
    struct chars path;        // ~/.searx/searx.json
    struct chars winner;
    struct text  plan;
    struct searx_insts insts;
    long fetched_at;          // UTC of last searx.space refresh
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

static bool searx_isdir(const char * p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool searx_isfile(const char * p) {
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

static bool searx_ask_yes(const char * q) {
    bool yes = false;
    if (isatty(fileno(stdin))) {
        fprintf(stderr, "%s [y/N] ", q);
        fflush(stderr);
        char b[16];
        if (fgets(b, sizeof(b), stdin) &&
            (b[0] == 'y' || b[0] == 'Y')) { yes = true; }
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
    bool got = false;
    const char * h = getenv("HOME");
    if (!h || !*h) { h = getenv("USERPROFILE"); }
    if (h && *h) {
        struct chars c = {0};
        chars_printf(&c, "%s/%s", h, SEARX_DIR_NAME);
        if (searx_isdir(c.data)) {
            chars_put(out, c.data, c.count);
            got = true;
        } else {
            char p[512];
            snprintf(p, sizeof(p), "Create %s/?", c.data);
            if (searx_ask_yes(p) && searx_mkdir(c.data) == 0) {
                chars_put(out, c.data, c.count);
                got = true;
            }
        }
        chars_free(&c);
    }
    if (!got) { chars_puts(out, searx_tmpdir()); }
}

static void searx_urlenc(struct chars * out, const char * s) {
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
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
                      const struct searx_args * a, bool as_json) {
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

static bool searx_contains(const struct chars * s, const char * n) {
    return searx_find(s->data, s->data + s->count, n) != NULL;
}

static const struct { const char * name; char ch; } searx_named[] = {
    {"quot", '"'}, {"apos", '\''}, {"amp", '&'},
    {"lt", '<'}, {"gt", '>'}, {"nbsp", ' '},
};

static bool searx_named_decode(struct chars * out,
                               const char * name, size_t n) {
    bool matched = false;
    size_t cnt = sizeof(searx_named) / sizeof(searx_named[0]);
    for (size_t i = 0; !matched && i < cnt; i++) {
        const char * en = searx_named[i].name;
        if (strlen(en) == n && memcmp(en, name, n) == 0) {
            chars_put(out, &searx_named[i].ch, 1);
            matched = true;
        }
    }
    return matched;
}

static uint32_t searx_num_decode(const char * s, const char * end) {
    uint32_t cp = 0;
    bool hex = (s < end && (*s == 'x' || *s == 'X'));
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
    bool in_tag = false;
    bool last_space = true;
    while (s < end) {
        char c = *s;
        if (in_tag) {
            if (c == '>') { in_tag = false; }
            s++;
        } else if (c == '<') {
            in_tag = true;
            s++;
        } else if (c == '&') {
            const char * semi = NULL;
            const char * q = s + 1;
            while (!semi && q < end && q - s < 12) {
                if (*q == ';') { semi = q; } else { q++; }
            }
            if (!semi) {
                chars_put(out, &c, 1);
                last_space = false;
                s++;
            } else {
                size_t n = (size_t)(semi - s - 1);
                if (n >= 2 && s[1] == '#') {
                    json_utf8(out, searx_num_decode(s + 2, semi));
                } else if (!searx_named_decode(out, s + 1, n)) {
                    chars_put(out, s, (size_t)(semi - s) + 1);
                }
                last_space = false;
                s = semi + 1;
            }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space) {
                chars_put(out, " ", 1);
                last_space = true;
            }
            s++;
        } else {
            chars_put(out, &c, 1);
            last_space = false;
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
                                         const char * url, bool create) {
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

static bool searx_skip(const struct searx_inst * e, long now) {
    if (e->checked_at > 0 && (now - e->checked_at) < SEARX_LOSE_TTL) {
        return true;  // recent visit, any outcome — anti-hammer floor
    }
    if (e->last_win_at > 0 && (now - e->last_win_at) < SEARX_WIN_TTL) {
        return true;  // recently served us — give it a rest
    }
    return false;
}

static void searx_load(struct searx_ctx * c) {
    struct chars buf = {0};
    if (searx_read_file(c->path.data, &buf) == 0) {
        struct json * j = json_parse(buf.data, buf.count);
        if (j) {
            c->fetched_at = (long)searx_jnum(j, "fetched_at", 0);
            struct json * arr = json_get(j, "instances");
            if (arr && arr->kind == JSON_ARR) {
                for (size_t i = 0; i < arr->u.arr.count; i++) {
                    struct json * it = arr->u.arr.data[i];
                    const char * url = searx_jstr(it, "url");
                    if (url) {
                        struct searx_inst * e =
                            searx_inst_at(&c->insts, url, true);
                        e->json = (int)searx_jnum(it, "json", 0);
                        e->html = (int)searx_jnum(it, "html", 0);
                        e->checked_at =
                            (long)searx_jnum(it, "checked_at", 0);
                        e->last_win_at =
                            (long)searx_jnum(it, "last_win_at", 0);
                    }
                }
            }
            json_free(j);
        }
    }
    chars_free(&buf);
}

static int searx_inst_cmp(const void * a, const void * b) {
    const struct searx_inst * x = a;
    const struct searx_inst * y = b;
    int r = y->json - x->json;
    return r != 0 ? r : y->html - x->html;
}

static void searx_save(struct searx_ctx * c) {
    qsort(c->insts.data, c->insts.count, sizeof(struct searx_inst),
          searx_inst_cmp);
    struct chars out = {0};
    chars_printf(&out, "{\n  \"fetched_at\":%ld,\n  \"instances\":[\n",
                 c->fetched_at);
    for (size_t i = 0; i < c->insts.count; i++) {
        struct searx_inst * e = &c->insts.data[i];
        if (i > 0) { chars_puts(&out, ",\n"); }
        chars_puts(&out, "    {\"url\":");
        struct json * t = json_new_str(e->url.data, e->url.count);
        json_write(t, &out);
        json_free(t);
        chars_printf(&out,
            ",\"json\":%d,\"html\":%d,"
            "\"checked_at\":%ld,\"last_win_at\":%ld}",
            e->json, e->html, e->checked_at, e->last_win_at);
    }
    chars_puts(&out, "\n  ]\n}\n");
    searx_write_file(c->path.data, out.data, out.count);
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
    chars_free(&c->path);
    chars_free(&c->winner);
    text_free(&c->plan);
    searx_insts_free(&c->insts);
}

static bool searx_passes_filter(struct json * v, const struct chars * key) {
    int sc = (int)searx_jnum(json_get(v, "http"), "status_code", 0);
    struct json * tm = json_get(v, "timing");
    double sp = searx_jnum(json_get(tm, "search_go"),
                           "success_percentage", -1);
    if (sp < 0) {
        sp = searx_jnum(json_get(tm, "search"),
                        "success_percentage", -1);
    }
    bool onion = false;
    size_t k = 0;
    while (!onion && k + 6 <= key->count) {
        if (memcmp(key->data + k, ".onion", 6) == 0) {
            onion = true;
        } else {
            k++;
        }
    }
    // sp > 0 (not > 90) — searx.space has ~80 instances total, 39 of
    // them at 0% success (truly dead). Anything searx.space has ever
    // seen succeed is worth keeping in the pool; our own per-host
    // counters take it from there.
    return !onion && sc == 200 && sp > 0.0 &&
           key->count > 8 &&
           memcmp(key->data, "https://", 8) == 0;
}

static struct json * searx_usable(struct chars * raw, bool as_json) {
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

// Informational message — appears once on its own line in BRIEF/PROGRESS/
// VERBOSE, suppressed in SILENT.
static void searx_say(int show, const char * fmt, ...) {
    if (show == SEARX_SHOW_SILENT) { return; }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void searx_attempt_begin(int show, size_t i, size_t lim,
                                const char * inst) {
    if (show == SEARX_SHOW_VERBOSE || show == SEARX_SHOW_BRIEF) {
        fprintf(stderr, "Attempt %zu/%zu: %s\n", i + 1, lim, inst);
    } else if (show == SEARX_SHOW_PROGRESS) {
        fprintf(stderr, "\r[%zu/%zu] %s ...\033[K", i + 1, lim, inst);
        fflush(stderr);
    }
}

static void searx_attempt_end(int show, size_t i, size_t lim,
                              const char * inst, bool ok,
                              bool via_json, bool reachable) {
    const char * status =
        ok ? (via_json ? "ok via json" : "ok via html")
           : (reachable ? "blocked" : "unreachable");
    if (show == SEARX_SHOW_VERBOSE || show == SEARX_SHOW_BRIEF) {
        fprintf(stderr, "  %s\n", status);
    } else if (show == SEARX_SHOW_PROGRESS) {
        fprintf(stderr, "\r[%zu/%zu] %s: %s\033[K%s",
                i + 1, lim, inst, status, ok ? "\n" : "");
        fflush(stderr);
    }
}

// Clear any partial PROGRESS line so subsequent stdout/stderr starts clean.
static void searx_progress_clear(int show) {
    if (show == SEARX_SHOW_PROGRESS) {
        fputs("\r\033[K", stderr);
        fflush(stderr);
    }
}

static bool searx_attempt(const char * inst, const struct searx_args * a,
                          bool as_json, bool * via_json, bool * reachable,
                          struct chars * raw, struct json ** parsed) {
    bool ok = false;
    *via_json = false;
    *reachable = false;
    struct chars url = {0};
    searx_url(&url, inst, a, as_json);
    chars_free(raw);
    if (a->show == SEARX_SHOW_VERBOSE) {
        fprintf(stderr, "  GET %s\n", url.data);
    }
    long st = 0;
    int rc = searx_http(url.data, a->timeout_ms, raw, &st);
    if (a->show == SEARX_SHOW_VERBOSE) {
        fprintf(stderr, "  -> HTTP %ld, %zu bytes\n", st, raw->count);
    }
    chars_free(&url);
    if (rc == 0) { *reachable = true; }
    if (rc == 0 && st == 200) {
        struct json * j = searx_usable(raw, as_json);
        if (j) {
            *parsed = j;
            *via_json = as_json;
            ok = true;
        }
    }
    return ok;
}

// Pick the protocol for this attempt. Default to what the user asked for
// (--json -> json, otherwise html). If the instance's history says it
// only ever served the other protocol, use that instead — both protocols
// land in the same internal JSON shape, so output formatting is unaffected.
static bool searx_pick_json(const struct searx_inst * e, bool human) {
    bool want_json = !human;
    bool as_json;
    if      (e->json > 0 && e->html == 0) { as_json = true; }
    else if (e->html > 0 && e->json == 0) { as_json = false; }
    else                                  { as_json = want_json; }
    return as_json;
}

static int searx_refresh(struct searx_ctx * c, int show) {
    int r = -1;
    searx_say(show, "Refreshing instance list from searx.space...");
    struct chars body = {0};
    long st = 0;
    if (searx_http(SEARX_REFRESH_URL, SEARX_REFRESH_MS, &body, &st) == 0 &&
        st == 200 && body.count > 0) {
        struct json * j = json_parse(body.data, body.count);
        struct json * inst = j ? json_get(j, "instances") : NULL;
        if (inst && inst->kind == JSON_OBJ) {
            size_t before = c->insts.count;
            for (size_t i = 0; i < inst->u.obj.count; i++) {
                struct chars * key = inst->u.obj.data[i].key;
                struct json * v = inst->u.obj.data[i].val;
                if (searx_passes_filter(v, key)) {
                    size_t n = key->count;
                    while (n > 0 && key->data[n - 1] == '/') { n--; }
                    struct chars u = {0};
                    chars_put(&u, key->data, n);
                    searx_inst_at(&c->insts, u.data, true);
                    chars_free(&u);
                }
            }
            c->fetched_at = time(NULL);
            searx_say(show, "  %zu instance(s) total (%zu new)",
                      c->insts.count, c->insts.count - before);
            r = 0;
        }
        json_free(j);
    }
    chars_free(&body);
    if (r != 0) { searx_say(show, "  refresh failed"); }
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

// When the plan is empty (every instance is in some cooldown), find
// the earliest UTC moment at which any host will become eligible.
// Returns -1 if there are no instances at all.
static long searx_next_eligible(const struct searx_insts * s, long now) {
    long best = -1;
    for (size_t i = 0; i < s->count; i++) {
        const struct searx_inst * e = &s->data[i];
        long lose_at = e->checked_at  > 0 ? e->checked_at  + SEARX_LOSE_TTL : now;
        long win_at  = e->last_win_at > 0 ? e->last_win_at + SEARX_WIN_TTL  : now;
        long at = lose_at > win_at ? lose_at : win_at;
        if (best < 0 || at < best) { best = at; }
    }
    return best;
}

// Build the attempt order. Two tiers — hosts that have served us before
// (json+html > 0) come first, fresh/unknown hosts second; we shuffle
// within each tier so load is spread but proven hosts win on tie.
// Anything visited within SEARX_LOSE_TTL (uniform) or whose last win is
// within SEARX_WIN_TTL is skipped outright.
static void searx_plan(struct text * plan, struct searx_insts * insts,
                       long now) {
    struct text good = {0};
    struct text fresh = {0};
    for (size_t i = 0; i < insts->count; i++) {
        struct searx_inst * e = &insts->data[i];
        if (searx_skip(e, now)) { continue; }
        if (e->json + e->html > 0) {
            text_put(&good, e->url.data, e->url.count);
        } else {
            text_put(&fresh, e->url.data, e->url.count);
        }
    }
    searx_shuffle(&good);
    searx_shuffle(&fresh);
    for (size_t i = 0; i < good.count;  i++) { text_puts(plan, good.data[i]);  }
    for (size_t i = 0; i < fresh.count; i++) { text_puts(plan, fresh.data[i]); }
    text_free(&good);
    text_free(&fresh);
}

static bool searx_search(const struct searx_args * a,
                         const struct text * plan,
                         struct searx_insts * known,
                         long deadline,
                         struct json ** found,
                         bool * via_json,
                         struct chars * winner) {
    struct chars raw = {0};
    bool got = false;
    size_t lim = plan->count;
    if (lim > SEARX_MAX_ATTEMPTS) { lim = SEARX_MAX_ATTEMPTS; }
    for (size_t i = 0; !got && i < lim && time(NULL) < deadline; i++) {
        const char * inst = plan->data[i];
        long now = time(NULL);
        searx_attempt_begin(a->show, i, lim, inst);
        bool reachable = false;
        struct json * j = NULL;
        struct searx_inst * e = searx_inst_at(known, inst, true);
        e->checked_at = now;  // stamp before GET — the VISIT_TTL anchor
        bool as_json = searx_pick_json(e, a->human);
        bool ok = searx_attempt(inst, a, as_json, via_json,
                                &reachable, &raw, &j);
        if (ok) {
            *found = j;
            chars_puts(winner, inst);
            if (*via_json) { e->json++; } else { e->html++; }
            e->last_win_at = now;  // arms the WIN_TTL cooldown
            got = true;
        }
        searx_attempt_end(a->show, i, lim, inst, ok, *via_json, reachable);
    }
    if (!got && time(NULL) >= deadline) {
        searx_progress_clear(a->show);
        searx_say(a->show, "Time budget exhausted.");
    }
    chars_free(&raw);
    return got;
}

static void searx_emit_human_chars(struct chars * out, struct json * root) {
    struct json * results = json_get(root, "results");
    if (results && results->kind == JSON_ARR) {
        for (size_t i = 0; i < results->u.arr.count; i++) {
            struct json * it = results->u.arr.data[i];
            const char * t = searx_jstr(it, "title");
            const char * u = searx_jstr(it, "url");
            const char * c = searx_jstr(it, "content");
            chars_printf(out, "Title: %s\nURL:   %s\nSnippet: %s\n\n",
                         t ? t : "", u ? u : "", c ? c : "");
        }
    }
}

static void searx_emit_chars(struct chars * out, struct json * found,
                             bool human) {
    if (human) {
        searx_emit_human_chars(out, found);
    } else {
        json_write(found, out);
        chars_put(out, "\n", 1);
    }
}

static void searx_emit(FILE * f, struct json * found, bool human) {
    struct chars out = {0};
    searx_emit_chars(&out, found, human);
    fwrite(out.data, 1, out.count, f);
    chars_free(&out);
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
        "  --no-console-io       suppress all stderr output (scripted use)\n"
        "  --test                run searx_test() smoke test and exit\n"
        "  -h, --help            this help\n");
}

static int searx_args(struct searx_args * a, int argc, char ** argv) {
    a->category = "general";
    a->language = "en-US";
    a->time_range = "";
    a->human = true;
    a->timeout_ms = SEARX_TIMEOUT_MS;
    int r = 0;
    int i = 1;
    int qstart = -1;
    while (r == 0 && qstart < 0 && i < argc) {
        const char * s = argv[i];
        if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
            a->help = true;
            i++;
        } else if (strcmp(s, "--verbose") == 0) {
            a->verbose = true;
            i++;
        } else if (strcmp(s, "--json") == 0) {
            a->human = false;
            i++;
        } else if (strcmp(s, "--refresh-instances") == 0) {
            a->refresh = true;
            i++;
        } else if (strcmp(s, "--saved-instances") == 0) {
            a->saved_only = true;
            i++;
        } else if (strcmp(s, "--no-console-io") == 0) {
            a->no_console_io = true;
            i++;
        } else if (strcmp(s, "--test") == 0) {
            a->test = true;
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

// Derive the show mode from flags + tty state. Centralized so searx_run
// and searx_query agree on it.
static int searx_show_mode(const struct searx_args * a) {
    int show;
    if      (a->no_console_io)                       { show = SEARX_SHOW_SILENT; }
    else if (a->verbose)                             { show = SEARX_SHOW_VERBOSE; }
    else if (a->human && isatty(fileno(stderr)))     { show = SEARX_SHOW_PROGRESS; }
    else if (a->human)                               { show = SEARX_SHOW_BRIEF; }
    else                                             { show = SEARX_SHOW_SILENT; }
    return show;
}

// Single search pipeline used by both searx_run (CLI) and searx_query
// (library API). On success, *found receives the parsed result tree
// (caller must json_free) and *via_json indicates how it arrived.
static bool searx_do(const struct searx_args * a, struct json ** found,
                     bool * via_json) {
    struct searx_ctx c = {0};
    searx_dir(&c.dir);
    searx_path(&c.path, c.dir.data, SEARX_DATA_BASE);
    if (a->show == SEARX_SHOW_VERBOSE) {
        searx_say(a->show, "Data file: %s", c.path.data);
    }
    searx_load(&c);
    bool refreshed = false;
    bool saved_only = a->saved_only;
    if (a->refresh) {
        searx_refresh(&c, a->show);
        refreshed = true;
    }
    if (c.insts.count == 0 && !refreshed) {
        if (saved_only) {
            searx_say(a->show,
                "No saved instances yet; discovering this run.");
        }
        searx_refresh(&c, a->show);
        refreshed = true;
        saved_only = false;
    }
    long now = time(NULL);
    long deadline_s = (long)a->timeout_ms / 1000 * 6;
    if (deadline_s < SEARX_DEADLINE_S) { deadline_s = SEARX_DEADLINE_S; }
    long deadline = now + deadline_s;
    searx_plan(&c.plan, &c.insts, now);
    if (a->show == SEARX_SHOW_VERBOSE || a->show == SEARX_SHOW_BRIEF) {
        searx_say(a->show, "Plan: %zu of %zu instance(s)%s",
                  c.plan.count, c.insts.count,
                  saved_only ? ", saved-only" : "");
        if (c.plan.count == 0 && c.insts.count > 0) {
            long when = searx_next_eligible(&c.insts, now);
            long wait = when > now ? when - now : 0;
            if (wait > 0) {
                searx_say(a->show,
                    "  all hosts on cooldown — next eligible in %lds", wait);
            }
        }
    }
    bool got = searx_search(a, &c.plan, &c.insts, deadline,
                            found, via_json, &c.winner);
    if (!got && !saved_only && !refreshed &&
        time(NULL) < deadline &&
        searx_refresh(&c, a->show) == 0) {
        refreshed = true;
        text_free(&c.plan);
        searx_plan(&c.plan, &c.insts, time(NULL));
        if (a->show == SEARX_SHOW_VERBOSE || a->show == SEARX_SHOW_BRIEF) {
            searx_say(a->show, "Plan: %zu instance(s)", c.plan.count);
        }
        got = searx_search(a, &c.plan, &c.insts, deadline,
                           found, via_json, &c.winner);
    }
    searx_save(&c);
    searx_progress_clear(a->show);
    searx_ctx_free(&c);
    return got;
}

// Public library API: run one search and copy the formatted output into
// `out`. `json` selects machine-readable JSON vs. human-readable text.
// Returns true if any instance returned usable results, false otherwise.
// Always silent on stderr — embedders log their own way if they want to.
__attribute__((unused))
static inline bool searx_query(const char * query, struct chars * out,
                               bool json) {
    struct searx_args a = {0};
    a.category = "general";
    a.language = "en-US";
    a.time_range = "";
    a.human = !json;
    a.no_console_io = true;
    a.timeout_ms = SEARX_TIMEOUT_MS;
    a.show = SEARX_SHOW_SILENT;
    if (query) { chars_puts(&a.query, query); }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    struct json * found = NULL;
    bool via = false;
    bool ok = searx_do(&a, &found, &via);
    if (ok && found) { searx_emit_chars(out, found, a.human); }
    if (found) { json_free(found); }
    chars_free(&a.query);
    return ok;
}

// Smoke test: run a couple of canned queries through searx_query() and
// verify each returns non-empty output. Requires network and live SearXNG
// instances. Returns 0 on success, 1 on failure.
__attribute__((unused))
static int searx_test(int argc, char ** argv, char ** env) {
    (void)argc; (void)argv; (void)env;
    static const char * const cases[] = {
        "open source software",
        "claude anthropic",
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        struct chars out = {0};
        fprintf(stderr, "[%zu/%zu] searx_query(%s)\n", i + 1, n, cases[i]);
        bool ok = searx_query(cases[i], &out, false);
        if (ok && out.count > 0) {
            fprintf(stderr, "  ok — %zu bytes\n", out.count);
        } else {
            fprintf(stderr, "  FAIL — no results\n");
            rc = 1;
        }
        chars_free(&out);
    }
    fprintf(stderr, "searx_test: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

__attribute__((unused))
static inline int searx_run(int argc, char ** argv, char ** env) {
    struct searx_args a = {0};
    int rc = 0;
    if (searx_args(&a, argc, argv) != 0) {
        searx_help(stderr);
        rc = 2;
    }
    if (rc == 0 && a.help) { searx_help(stdout); }
    if (rc == 0 && !a.help && a.test) {
        rc = searx_test(argc, argv, env);
    }
    if (rc == 0 && !a.help && !a.test && a.query.count == 0) {
        searx_help(stderr);
        rc = 2;
    }
    if (rc == 0 && !a.help && !a.test && a.query.count > 0) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        a.show = searx_show_mode(&a);
        struct json * found = NULL;
        bool via = false;
        bool got = searx_do(&a, &found, &via);
        if (got && found) {
            searx_emit(stdout, found, a.human);
        } else {
            searx_say(a.show, "No results.");
            rc = 1;
        }
        if (found) { json_free(found); }
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
