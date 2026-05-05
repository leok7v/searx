# searx

A small SearXNG meta-search client in C. Wraps libcurl, parses JSON when
the instance offers it, scrapes the simple HTML theme when it doesn't.

Most public SearXNG instances no longer honor `format=json` for anonymous
clients (HTTP 429 from rate limiters, 403/401 from Anubis or Cloudflare,
or a 200 with an HTML body when JSON was requested). searx sends one
GET per attempt — no JSON-then-HTML double probe on the same host, since
that pattern reliably gets the client banned. By default human-readable
runs request the HTML theme directly; `--json` requests `format=json`. If
a host's history says it only ever served the other protocol, we use
that protocol instead — both land in the same internal result shape.

Two cooldowns drive the planner, both load-bearing:

* `SEARX_LOSE_TTL` (60 s) — uniform anti-hammer floor on every visit.
  Whether the host served us, blocked us, or was unreachable, we won't
  re-hit it for at least a minute.
* `SEARX_WIN_TTL` (10 min) — extra "be polite" delay only on hosts that
  just successfully served results, so each run rotates across the
  available pool instead of leaning on the same winner.

Per-host success counters (`json` / `html`) order the plan: proven hosts
first, fresh/unknown hosts second, shuffled within each tier.

The candidate pool comes from `searx.space`. We accept any host that has
ever returned a successful search (filter: `https://`, non-onion, home
page returns 200, search success rate > 0%). That picks up roughly 30
of the ~80 hosts searx.space tracks — the rest are dead, onion-only,
or have never answered.

When every host is on cooldown the verbose log says so and prints how
many seconds until the next one is eligible — so an empty-plan run
isn't silent.

## Build

    make

That's it. Or by hand:

    cc -O2 src/searx.c -lcurl -o searx

Tested on macOS with the system toolchain. Should work on Linux
unchanged and on Windows with libcurl available.

Run it:

    ./searx "your query here"

## Usage

    searx [options] [--] <query>

    --category <c>        searx category, default "general"
    --time <t>            time range (day, week, month, year)
    --timeout <s>         per-instance HTTP timeout (default 5)
    --json                JSON output (default: human-readable)
    --refresh-instances   force-refresh searx.space cache
    --saved-instances     only try previously-known reliable instances
                          (auto-refreshes once if the saved list is empty)
    --verbose             full multi-line debug on stderr
    --no-console-io       suppress all stderr output (scripted use)
    --test                run the smoke test and exit
    -h, --help            this help

The query can contain spaces. Use `--` to stop option parsing if your
query starts with a dash.

### Progress reporting

Default human-mode picks the right stderr style automatically:

* TTY stderr → single-line `\r`-overwriting progress (`[ 3/12] host: ok`)
* non-TTY stderr (piped) → brief multi-line "Attempt N/M" log
* `--verbose` → full debug per attempt (URLs, status codes, byte counts)
* `--no-console-io` → silent
* `--json` (without `--verbose`) → silent

## Where state goes

On first run searx asks whether to create `~/.searx/`. If you say no, or
the process isn't on a TTY, it falls back to `$TMPDIR` / `$TMP` /
`$TEMP` / `/tmp`. A single file lives in that directory:

    ~/.searx/searx.json

Schema:

    {
      "fetched_at": <utc>,
      "instances": [
        {"url": "...", "json": N, "html": M,
         "checked_at": <utc>, "last_win_at": <utc>},
        ...
      ]
    }

`fetched_at` is the UTC of the last successful searx.space refresh.
Each instance carries a count of past JSON / HTML successes (used to
order the plan), `checked_at` (UTC of the last attempt of any kind —
arms `SEARX_LOSE_TTL`), and `last_win_at` (UTC of the last successful
response — arms `SEARX_WIN_TTL`). Older state files without
`last_win_at` load with the field defaulted to 0 and pick it up on the
next win.

## Embedding

`searx.c` is a single-file library. Define `SEARX_LIB` before including
it to drop its `main()` and expose two entry points:

    #define SEARX_LIB
    #include "searx.c"

    // Full CLI behaviour: parses argv, writes results to stdout,
    // returns process exit code.
    int  searx_run  (int argc, char ** argv, char ** env);

    // Library call: drives the same pipeline silently and returns
    // formatted output as bytes in `out`. Returns true if any
    // results were found.
    bool searx_query(const char * query, struct chars * out, bool json);

`src/searx_example.c` shows both forms — pass `--lib "your query"` to
exercise `searx_query()`, or run it like the regular CLI to fall through
to `searx_run()`.
