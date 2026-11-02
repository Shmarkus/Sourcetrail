# st serve: results

Wall-clock medians of 10 runs, in milliseconds unless noted. "Cold" is one `st` process per call (`ST_NO_SERVER=1`);
"warm" goes through `st serve`. Answers are identical in both cases.

## Targets

| Target | Result |
|---|---|
| Startup under 200 ms | Met warm for `status` (110 ms) and `refs` of a library symbol (110 ms) on the 2k-file service; `callers` 144 ms. Cold is 0.4-0.6 s. |
| Query under 1 s | Met cold and warm. |
| Refresh under 5 s for a few changed files | Not met: the indexer still takes about 10-12 s on the service. With the watcher running it happens in the background, so the query that follows an edit does not wait for it. |

## Smoke fixture (`testing/cli/lombok`, indexed as in `testing/cli/st-smoke.sh`)

| Query | Cold | Warm |
|---|---|---|
| `st status` | 10 | 8 |
| `st refs example.Person` | 5 | 6 |
| `st callers 'example.Caller.greeting(example.Person)'` | 6 | 5 |

The fixture is a handful of files, so process start dominates and the server gains nothing. The point of the server shows on a large index.

## A private 2k-file Spring service

| Query | Cold before | Cold now | Warm first cut | Warm now |
|---|---|---|---|---|
| `st status` | 780 | 430 (479 in a later run) | 179 | 110 |
| `st refs Scheduled` (library symbol) | 990 | 540 (575) | 316 | 110 |
| `st callers <Class>.<method>` | 860 | 510 (489) | 226 | 144 |

- "Cold before/now" is the cheaper cold path (the library-name pass no longer repeats the fuzzy search; median of 5, `--no-refresh`). "Warm first cut" is the first server; "warm now" adds memoised unresolved counts, library names, Lombok roots and gap inputs.
- `st refs Scheduled` now resolves library symbols.
- After an edit with the watcher running, the background refresh took 10.4-12.5 s. The first query 15 s later was as fast as a steady warm one (`status` 117-121 ms against 108-115 ms steady). Without the watcher the same query first runs a refresh of about 11.5 s.
- A query that arrives while a refresh is running waits for it and says so on stderr (`st: waited Xs for the background refresh`).

## Next steps

- The 10-12 s `Sourcetrail index` refresh itself: process start, the JVM, the Gradle fingerprint and delombok. The server moves it off the agent's path but does not shorten it. Profiling it is the next plan.
- `st batch`: a warm server makes per-call startup small. Revisit only if measurements say otherwise.
- One server serving several projects.
- Smaller items: the freshness walk (about 57 ms) still opens a second storage per request; a first query on a new heavily used symbol pays about 60 ms once.
