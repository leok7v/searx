// Embed searx.c as a single-file library: SEARX_LIB drops its main()
// and exposes searx_run() and searx_query() for embedding.
#define SEARX_LIB
#include "searx.c"

// Two ways to embed:
//
//   1. searx_run(argc, argv, env) — full CLI behaviour (parses flags,
//      writes formatted results to stdout, returns process exit code).
//      Useful when you want to ship the binary as-is.
//
//   2. searx_query(query, &out, json) — library call. Drives the same
//      pipeline silently and hands back the formatted output as bytes
//      in `out`. Returns true if any results were found.
//
// To try the library API: ./searx_example --lib "your search terms"
// Anything else falls through to the regular CLI.
int main(int argc, char * argv[], char * env[]) {
    if (argc >= 3 && strcmp(argv[1], "--lib") == 0) {
        struct chars out = {0};
        const char * q = argv[2];
        bool json = (argc >= 4 && strcmp(argv[3], "--json") == 0);
        bool ok = searx_query(q, &out, json);
        if (ok) {
            fwrite(out.data, 1, out.count, stdout);
        } else {
            fprintf(stderr, "searx_query: no results for \"%s\"\n", q);
        }
        chars_free(&out);
        return ok ? 0 : 1;
    }
    return searx_run(argc, argv, env);
}
