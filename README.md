# searx

A small SearXNG meta-search client in C. Wraps libcurl, parses JSON when
the instance offers it, scrapes HTML when it doesn't.

Most public SearXNG instances no longer honor `format=json` for anonymous
clients (HTTP 429 from rate limiters, 403/401 from Anubis or Cloudflare,
or a 200 with HTML body when JSON was requested). searx tries JSON first,
falls back to scraping the simple theme on the same instance, and
escalates to refreshing the instance list and trying again. It keeps a
per-instance success counter so the next run starts with the ones that
actually answered.

## Build

    make

That's it. Or by hand:

    cc -O2 src/searx.c -lcurl -o searx

The Makefile does the same thing. Tested on macOS with the system
toolchain. Should work on Linux unchanged and on Windows with libcurl
available.

Run it:

    ./searx --verbose -h "your query here"

## Usage

    searx [options] [--] <query>

    --category <c>        searx category, default "general"
    --time <t>            time range (day, week, month, year)
    -h                    human-readable output
    --json                JSON output (default)
    --refresh-instances   force-refresh searx.space cache
    --saved-instances     only try previously-known reliable instances
                          (auto-refreshes once if the saved list is empty)
    --verbose             progress on stderr
    --help                this help

The query can contain spaces. Use `--` to stop option parsing if your
query starts with a dash.

## Where state goes

On first run searx asks whether to create `~/.searx/`. If you say no, or
the process isn't on a TTY, it falls back to `$TMPDIR` / `$TMP` /
`$TEMP` / `/tmp`. Two files live in that directory:

| file                                  | what                                                   |
|---------------------------------------|--------------------------------------------------------|
| `searx.space.data.instances`          | last fetched searx.space dump                          |
| `searx.space.data.instances.reliable` | per-instance counters and reachability, JSON-capable on top |

The reliable list is consulted first on every run, sorted by JSON
successes then HTML successes. Each entry also carries a last-seen state
and timestamp so instances that just failed to connect are skipped until
the TTL elapses.

## Embedding

`searx.c` is a single-file library. Define `SEARX_LIB` before including
it to drop its `main()` and just get `int searx_run(int, char **, char **)`:

    #define SEARX_LIB
    #include "searx.c"

    int main(int argc, char * argv[], char * env[]) {
        return searx_run(argc, argv, env);
    }

That is exactly what `src/searx_example.c` does.
