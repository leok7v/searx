#ifndef CURLY_C
#define CURLY_C

// curly.c: thin libcurl wrapper. One curly = one CURL easy handle
// attached to one CURLM (multi) handle; one in-flight request at a
// time per curly.
//
// Lifts libcurl up to a "send + wait + recv" shape that resembles
// non-blocking sockets, while inheriting from libcurl:
//   - cross-platform TLS (Schannel on Windows, SecureTransport or
//     OpenSSL on macOS, OpenSSL on Linux)
//   - HTTP/1.1 keep-alive (default), HTTP/2 (opt-in), HTTP/3 (opt-in
//     only if libcurl was built with HTTP/3 support)
//   - chunked Transfer-Encoding decode (automatic)
//   - gzip/deflate/br response decompression (when CURLOPT_ACCEPT_
//     ENCODING set; default off here)
//   - DNS, redirects (off by default), timeouts, connection pool
//
// Out of scope on purpose:
//   - server side TLS (libcurl is client-only; use a reverse proxy)
//   - HTTP/2 multiplexing across many concurrent requests on one
//     curly (one easy handle, one in-flight request)
//   - cookies, redirects, retries (caller adds via flags if needed)
//   - select-style multiplexing across many curlys (each curly has
//     its own multi; for cross-curly polling, share a multi handle
//     externally - not v1)

#include "chars.c"
#include "text.c"

#include <curl/curl.h>

struct curly {
    CURLM * multi;
    CURL * easy;
    struct curl_slist * req_headers;
    struct chars body;
    struct text resp_hdr_names;
    struct text resp_hdr_values;
    long status;
    int active;
    int done;
    int err;
    size_t body_read;
};

static inline int curly_global_init_(void) {
    static int done = 0;
    int ok = 1;
    if (!done) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
            done = 1;
        } else {
            ok = 0;
        }
    }
    return ok;
}

static size_t curly_write_cb_(char * data, size_t size, size_t nmemb,
                              void * userp) {
    struct curly * c = userp;
    size_t n = size * nmemb;
    chars_put(&c->body, data, n);
    return n;
}

static size_t curly_header_cb_(char * data, size_t size, size_t nmemb,
                               void * userp) {
    struct curly * c = userp;
    size_t n = size * nmemb;
    size_t end = n;
    while (end > 0
           && (data[end - 1] == '\r' || data[end - 1] == '\n')) {
        end--;
    }
    size_t i = 0;
    while (i < end && data[i] != ':') { i++; }
    if (i > 0 && i < end) {
        struct chars name = {0};
        chars_put(&name, data, i);
        size_t vs = i + 1;
        while (vs < end && data[vs] == ' ') { vs++; }
        struct chars value = {0};
        chars_put(&value, data + vs, end - vs);
        text_puts(&c->resp_hdr_names, name.data);
        text_puts(&c->resp_hdr_values, value.data);
        chars_free(&name);
        chars_free(&value);
    }
    return n;
}

__attribute__((unused))
static inline int curly_init(struct curly * c) {
    int ok = 1;
    memset(c, 0, sizeof(*c));
    if (!curly_global_init_()) { ok = 0; }
    if (ok) {
        c->multi = curl_multi_init();
        if (!c->multi) { ok = 0; }
    }
    if (ok) {
        c->easy = curl_easy_init();
        if (!c->easy) { ok = 0; }
    }
    if (ok) {
        curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION,
                         curly_write_cb_);
        curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c);
        curl_easy_setopt(c->easy, CURLOPT_HEADERFUNCTION,
                         curly_header_cb_);
        curl_easy_setopt(c->easy, CURLOPT_HEADERDATA, c);
        curl_easy_setopt(c->easy, CURLOPT_NOSIGNAL, 1L);
    }
    if (!ok) {
        if (c->easy) { curl_easy_cleanup(c->easy); c->easy = NULL; }
        if (c->multi) { curl_multi_cleanup(c->multi); c->multi = NULL; }
    }
    return ok ? 0 : -1;
}

__attribute__((unused))
static inline void curly_reset(struct curly * c) {
    if (c->active && c->easy && c->multi) {
        curl_multi_remove_handle(c->multi, c->easy);
        c->active = 0;
    }
    if (c->req_headers) {
        curl_slist_free_all(c->req_headers);
        c->req_headers = NULL;
    }
    chars_free(&c->body);
    text_free(&c->resp_hdr_names);
    text_free(&c->resp_hdr_values);
    c->status = 0;
    c->done = 0;
    c->err = 0;
    c->body_read = 0;
    if (c->easy) { curl_easy_reset(c->easy); }
    if (c->easy) {
        curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION,
                         curly_write_cb_);
        curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c);
        curl_easy_setopt(c->easy, CURLOPT_HEADERFUNCTION,
                         curly_header_cb_);
        curl_easy_setopt(c->easy, CURLOPT_HEADERDATA, c);
        curl_easy_setopt(c->easy, CURLOPT_NOSIGNAL, 1L);
    }
}

__attribute__((unused))
static inline void curly_close(struct curly * c) {
    if (c->active && c->easy && c->multi) {
        curl_multi_remove_handle(c->multi, c->easy);
        c->active = 0;
    }
    if (c->req_headers) {
        curl_slist_free_all(c->req_headers);
        c->req_headers = NULL;
    }
    if (c->easy) { curl_easy_cleanup(c->easy); c->easy = NULL; }
    if (c->multi) { curl_multi_cleanup(c->multi); c->multi = NULL; }
    chars_free(&c->body);
    text_free(&c->resp_hdr_names);
    text_free(&c->resp_hdr_values);
}

__attribute__((unused))
static inline void curly_add_header(struct curly * c,
                                    const char * k, const char * v) {
    struct chars line = {0};
    chars_printf(&line, "%s: %s", k, v);
    c->req_headers = curl_slist_append(c->req_headers, line.data);
    chars_free(&line);
}

__attribute__((unused))
static inline void curly_set_timeout_ms(struct curly * c, long ms) {
    if (c->easy) { curl_easy_setopt(c->easy, CURLOPT_TIMEOUT_MS, ms); }
}

__attribute__((unused))
static inline void curly_set_no_verify(struct curly * c) {
    if (c->easy) {
        curl_easy_setopt(c->easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c->easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

__attribute__((unused))
static inline void curly_set_http2(struct curly * c) {
    if (c->easy) {
        curl_easy_setopt(c->easy, CURLOPT_HTTP_VERSION,
                         (long)CURL_HTTP_VERSION_2TLS);
    }
}

__attribute__((unused))
static inline int curly_set_http3(struct curly * c) {
    int ok = 0;
    if (c->easy) {
#ifdef CURL_HTTP_VERSION_3
        CURLcode r = curl_easy_setopt(c->easy, CURLOPT_HTTP_VERSION,
                                      (long)CURL_HTTP_VERSION_3);
        if (r == CURLE_OK) { ok = 1; }
#endif
    }
    return ok;
}

typedef size_t (*curly_writer)(char * data, size_t size, size_t nmemb,
                               void * userp);

__attribute__((unused))
static inline void curly_set_writer(struct curly * c,
                                    curly_writer cb, void * data) {
    if (c->easy) {
        curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION, cb);
        curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, data);
    }
}

typedef int (*curly_progress)(void * clientp,
                              curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);

__attribute__((unused))
static inline void curly_set_progress(struct curly * c,
                                      curly_progress cb, void * data) {
    if (c->easy) {
        curl_easy_setopt(c->easy, CURLOPT_XFERINFOFUNCTION, cb);
        curl_easy_setopt(c->easy, CURLOPT_XFERINFODATA, data);
        curl_easy_setopt(c->easy, CURLOPT_NOPROGRESS, 0L);
    }
}

__attribute__((unused))
static inline int curly_send(struct curly * c, const char * url,
                             const char * method,
                             const char * body, size_t body_n) {
    int ok = 1;
    if (!c->easy || !c->multi) { ok = 0; }
    if (ok) {
        curl_easy_setopt(c->easy, CURLOPT_URL, url);
        if (method && strcmp(method, "GET") != 0) {
            curl_easy_setopt(c->easy, CURLOPT_CUSTOMREQUEST, method);
        }
        if (body && body_n > 0) {
            curl_easy_setopt(c->easy, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(c->easy, CURLOPT_POSTFIELDSIZE,
                             (long)body_n);
        }
        if (c->req_headers) {
            curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER,
                             c->req_headers);
        }
        CURLMcode mc = curl_multi_add_handle(c->multi, c->easy);
        if (mc != CURLM_OK) { ok = 0; }
        else { c->active = 1; }
    }
    return ok ? 0 : -1;
}

__attribute__((unused))
static inline int curly_wait(struct curly * c, int timeout_ms) {
    int ok = 1;
    if (!c->multi) { ok = 0; }
    if (ok && !c->done) {
        int numfds = 0;
        CURLMcode mc = curl_multi_poll(c->multi, NULL, 0,
                                       timeout_ms, &numfds);
        if (mc != CURLM_OK) { ok = 0; }
    }
    return ok ? 0 : -1;
}

__attribute__((unused))
static inline ssize_t curly_recv(struct curly * c, char * buf,
                                 size_t n) {
    ssize_t r = 0;
    if (!c->multi || !c->easy) {
        r = -1;
    } else {
        int still_running = 0;
        CURLMcode mc = curl_multi_perform(c->multi, &still_running);
        if (mc != CURLM_OK) {
            c->err = 1;
            r = -1;
        } else {
            int msgs = 0;
            CURLMsg * m = curl_multi_info_read(c->multi, &msgs);
            while (m) {
                if (m->msg == CURLMSG_DONE) {
                    c->done = 1;
                    if (m->data.result != CURLE_OK) { c->err = 1; }
                    curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE,
                                      &c->status);
                }
                m = curl_multi_info_read(c->multi, &msgs);
            }
            size_t avail = c->body.count - c->body_read;
            size_t take = avail < n ? avail : n;
            if (take > 0) {
                memcpy(buf, c->body.data + c->body_read, take);
                c->body_read += take;
                r = (ssize_t)take;
            } else if (c->done) {
                r = 0;
            } else {
                r = 0;
            }
        }
    }
    return r;
}

__attribute__((unused))
static inline int curly_do(struct curly * c, const char * url,
                           const char * method,
                           const char * body, size_t body_n) {
    int rc = curly_send(c, url, method, body, body_n);
    int ok = (rc == 0);
    while (ok && !c->done) {
        char tmp[8192];
        ssize_t got = curly_recv(c, tmp, sizeof(tmp));
        if (got < 0) {
            ok = 0;
        } else if (got == 0 && !c->done) {
            if (curly_wait(c, 100) != 0) { ok = 0; }
        }
    }
    if (ok && c->err) { ok = 0; }
    return ok ? 0 : -1;
}

__attribute__((unused))
static inline long curly_status(const struct curly * c) {
    return c->status;
}

__attribute__((unused))
static inline const char * curly_resp_header(const struct curly * c,
                                             const char * name) {
    const char * r = NULL;
    int done = 0;
    size_t need = strlen(name);
    for (size_t i = 0; !done && i < c->resp_hdr_names.count; i++) {
        const char * hn = c->resp_hdr_names.data[i];
        size_t got = strlen(hn);
        if (got == need) {
            size_t k = 0;
            int eq = 1;
            while (eq && k < got) {
                char a = hn[k];
                char b = name[k];
                if (a >= 'A' && a <= 'Z') { a = (char)(a + 32); }
                if (b >= 'A' && b <= 'Z') { b = (char)(b + 32); }
                if (a != b) { eq = 0; }
                k++;
            }
            if (eq) {
                r = c->resp_hdr_values.data[i];
                done = 1;
            }
        }
    }
    return r;
}

#ifdef CURLY_TESTS

#include <assert.h>

static void test_curly_init_close(void) {
    struct curly c;
    assert(curly_init(&c) == 0);
    assert(c.multi != NULL);
    assert(c.easy != NULL);
    curly_close(&c);
}

static void test_curly_setup(void) {
    struct curly c;
    assert(curly_init(&c) == 0);
    curly_set_timeout_ms(&c, 5000);
    curly_set_no_verify(&c);
    curly_set_http2(&c);
    curly_add_header(&c, "X-Test", "value");
    curly_add_header(&c, "User-Agent", "curly/0.1");
    assert(c.req_headers != NULL);
    curly_close(&c);
}

static void test_curly_resp_header(void) {
    struct curly c;
    assert(curly_init(&c) == 0);
    text_puts(&c.resp_hdr_names, "Content-Type");
    text_puts(&c.resp_hdr_values, "application/json");
    text_puts(&c.resp_hdr_names, "X-Foo");
    text_puts(&c.resp_hdr_values, "bar");
    const char * v = curly_resp_header(&c, "content-type");
    assert(v && strcmp(v, "application/json") == 0);
    v = curly_resp_header(&c, "X-FOO");
    assert(v && strcmp(v, "bar") == 0);
    v = curly_resp_header(&c, "missing");
    assert(v == NULL);
    curly_close(&c);
}

static void test(void) {
    test_curly_init_close();
    test_curly_setup();
    test_curly_resp_header();
}

int main(int argc, char * argv[], char * env[]) {
    (void)argc; (void)argv; (void)env;
    test();
    printf("OK\n");
    return 0;
}

#endif /* CURLY_TESTS */

#endif /* CURLY_C */
