# Upgrade port: switch-gate results

The fork (Java indexing and `st`) was ported onto upstream's Conan/Qt 6/C++20 base (design:
`2026-11-02-upgrade-port-design.md`). Before the new build replaces master, it had to index the same as master, answer
the agent benchmark as well, and be no slower. Measured on Nov 3, 2026, on a private 2k-file Spring/Gradle service with
Lombok (1416 Java source files), unless noted.

## Index comparison (`testing/port/compare-index.sh`)

Each build indexes its own copy of the project. The script then compares sorted node names, edges (type, source,
target), source locations (relative file, start, end, type) and errors.

| Project | Mode | Result |
|---|---|---|
| The private service | full | identical, 669,590 rows (1416/1416 files, 123 errors on both) |
| The private service | one method added to a use-case class with dependents, then incremental | identical, 669,599 rows (3 files re-indexed on both) |
| `testing/cli/lombok` | full | identical, 313 rows |
| `testing/cli/maven` | full | identical, 194 rows |
| spring-boot-realworld-example-app @ `ee17e31` | full | identical, 32,266 rows (116 files, 258 errors on both) |

The 123 errors are the same on both builds. Many are uses of JPA metamodel classes (`Order_`), which an annotation
processor generates during the build (see Known gaps).

## Agent benchmark (`testing/benchmark`, `bench.py run`)

9 questions, 3 runs each, with fresh headless Claude Code agents and the default model, billed to a subscription (no
API key in the environment). `st serve` was running and warm. Only the `st` arm was run with the new build. It is
compared with the `st` arm of master's run of Nov 2 (Claude Code 2.1.316; 2.1.317 on Nov 3). The grep arm does not
depend on the build, so it was not run again. The agent note (the CLAUDE.md snippet and the README `st` section) was
the same text in both runs.

| `st` arm | master, Nov 2 | new build, Nov 3 | Change |
|---|---|---|---|
| Correct runs | 27 / 27 | 27 / 27 | |
| Partial, wrong or failed runs | 0 | 0 | |
| Tokens per question (median) | 50,306 | 53,942 | +7.2% |
| Tokens, all 27 runs | 1,522,633 | 1,467,010 | -3.7% |
| Context growth per question (median) | 6,208 | 4,835 | -22% |
| Tool calls per question (median) | 3 | 3 | 0 |
| Time per question (median) | 14.4 s | 15.7 s | +9% (the machine was under load, see below) |
| Tool output characters | 346,963 | 284,585 | -18% |
| `st` used on structural and stack-trace runs | 18 / 21 | 21 / 21 | |
| `st` used on plain-text runs | 1 / 6 | 2 / 6 | |

Most of the rise in median tokens comes from one question. Agents answered it with grep on master (0 of 3 runs used
`st`) and with `st` on the new build (3 of 3), at about 16k more tokens each. For reference, master's grep arm of Nov 2
had a median of 65,440 tokens, 4 tool calls and 16.4 s.

## Speed (`bench.py speed`, warm `st serve`)

The machine was under load on Nov 3: load average 3 to 5 on 20 cores, from an IDE, browsers and a language server,
with all of swap in use. Master's own frozen binaries ran 25 to 50% slower than on Nov 2. So master was measured again
in the same session, on its own copy of the project with the same settings, interleaved with the new build.

| Measure | master, Nov 2 | master, Nov 3 (same session) | new build, Nov 3 | Target |
|---|---|---|---|---|
| Startup, `st status` | 83 ms | 108–114 ms | 87–89 ms | under 200 ms |
| Query, median over the questions | 0.136 s | 0.186–0.197 s | 0.153–0.159 s | under 1 s |
| Query, slowest (`st at … \| st callers --depth 3`) | 0.601 s | 0.811–0.892 s | 0.778–0.784 s | under 1 s |
| First query after a one-line edit | 8.4 s | 10.5–11.5 s | 10.5–11.5 s | under 5 s |

Ranges span the separate runs. The last run of the new build, after the console-output changes, gave 89 ms, 0.159 s,
0.780 s and 10.50 s, against 114 ms, 0.197 s, 0.892 s and 11.54 s for master in the same session.

Without a server, measured interleaved in the same session: `st status` 0.45 s (master 0.46 s), median query 0.59 s
(0.71 s), slowest 1.38 s (1.40 s). A full index took 2:51 (master 3:09). The new build's SQLite uses 4 KB pages
(master 1 KB), which helps storage-heavy queries.

A refresh after a one-line edit spends about 4.6 s on Gradle, delombok of the changed file and the class path, the
same on both builds. Process lifetimes move in whole seconds, from the indexer's 1 s start delay and the shutdown tick.
That is why single refresh runs differ by 1 s.

## Gate

| Gate line | Result | Verdict |
|---|---|---|
| Index identical to master (full and after an edit) | identical on all five projects | pass |
| Correct answers ≥ 27 / 27 | 27 / 27 | pass |
| Tokens within 10% of master | median +7.2%, total -3.7% | pass |
| Tool calls within 10% of master | median 3 against 3 | pass |
| Median query within 10% of master | 0.159 s against 0.197 s for master in the same session | pass |
| Slowest query within 10% of master | 0.780 s against 0.892 s for master in the same session | pass |
| After an edit, no slower than master | 10.50 s against 11.54 s for master in the same session | pass |

The speed lines are judged against master measured in the same session. Against master's Nov 2 numbers they miss by
+17%, +29% and +37%, the same amount that master itself misses against those numbers on Nov 3. The new build is as fast
as master or faster on every measure taken in the same session. The refresh target of under 5 s is still not met, by
either build.

## Known gaps

- Classes an annotation processor generates during the build (JPA metamodel `Order_`, MapStruct `...MapperImpl`) are not
  indexed, as on master. Uses of them count as index errors.
- A plain Java project (one without Gradle or Maven, created from a folder) stores its source folder as an absolute path,
  so moving the checkout breaks it. Gradle and Maven projects store relative paths.
- Python support is not built. It returns in a later release.
- C/C++ indexing is off, and only Linux is built.
- A refresh after an edit takes about 10 s on a large Gradle project (target: under 5 s). With `st serve` watching the
  sources it happens in the background, so the next query does not wait for it.
