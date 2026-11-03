# Code-navigation tools for agents: st, IntelliJ MCP and jdtls against grep

Date: 2026-11-03. Branch: `bench/agent-benchmark` (harness at 7aede5bdf). st build: master e5950860b, frozen.

## Purpose

Find out which tool agents should use for tracing in our Java codebases, and whether the st fork is worth
maintaining. The agent benchmark (`testing/benchmark/`) was run with four arms on the same questions, with the same
measurements, against the targets in `2026-10-30-sourcetrail-cli-requirements.md`:

- at least 50% fewer tokens, context growth and tool calls per question than grep,
- at least 30% faster per question, start to answer,
- correctness no worse than grep.

## Method

- **Project:** a private 2k-file Spring service (14 Gradle modules, heavy Lombok, Java 21). The public realworld set
  was written and smoke-tested but not run in full (scope decision).
- **Questions:** 11, built with grep and reading before any tool run. 7 multi-hop: entry points that reach a method
  through interfaces, a scheduled job and a message-consumer bean; three paths through Spring beans, one through a
  list of strategy beans; the impact of a signature change, callers and controllers above; and a stack trace. 2
  one-hop controls and 2 text controls. By kind: 8 structural, 1 stack trace, 2 text. A twelfth question was
  dropped[^1].
- **Arms** (one config file each, `testing/benchmark/arms/`). Every arm has Bash, Read, Grep and Glob, its tools
  allowed up front (`--allowedTools`, no permission skipping), only its own MCP servers, and a project note of the
  same shape and length as the st snippet.

  | Arm | Working copy | Own tool |
  |---|---|---|
  | grep | clean copy | none |
  | st | the indexed project, frozen st, warm `st serve` | `st` through Bash |
  | intellij | clean copy, opened by a headless IntelliJ IDEA 2026.2.3 (`idea.sh mcpServer`, own config dir) | its MCP server, 10 navigation and search tools; the server refuses every other tool |
  | jdtls | clean copy | Claude Code's `LSP` tool through the jdtls-lsp plugin (jdtls 1.58, Lombok agent) |
- **Runs:** 11 questions x 4 arms x 3 repeats = 132 fresh `claude -p` agents, Claude Code 2.1.317, model
  claude-opus-5-5 in every run, subscription billing. Repeat 1 of every question ran before repeat 2. The arm that
  goes first rotates per question.
- **Warm-up** is reported apart and is not in any question time. Per-session startup is measured apart too.
- **Checks:** 0 failed runs, 0 permission denials, and 0 runs whose commands or paths touched question or result
  files.

## Results against the targets

Medians per question; the change is against grep.

| Metric | grep | st | intellij | jdtls | Target |
|---|---|---|---|---|---|
| Correct runs | 33 / 33 | 33 / 33 | 33 / 33 | 33 / 33 | no worse than grep |
| Tokens | 81k | 41k (-49%) | 98k (+21%) | 127k (+57%) | -50% |
| Context growth | 10.2k | 3.9k (-62%) | 8.4k (-18%) | 9.6k (-6%) | -50% |
| Tool calls | 6 | 2 (-67%) | 4 (-33%) | 8 (+33%) | -50% |
| Time, start to answer | 23.7 s | 16.3 s (-31%) | 20.8 s (-12%) | 36.3 s (+53%) | -30% |
| Tool output characters (total) | 601k | 413k (-31%) | 588k (-2%) | 549k (-9%) | |
| Cost, USD, 33 runs (CLI-reported) | 4.17 | 3.18 | 4.65 | 5.40 | |
| Own-tool use, structural runs | | 27 / 27 | 25 / 27 | 25 / 27 | at least 80% |

**By question shape:**

| Shape (runs per arm) | Metric | grep | st | intellij | jdtls |
|---|---|---|---|---|---|
| Multi-hop (21) | tokens | 112k | 45k (-60%) | 125k (+12%) | 157k (+41%) |
| | context growth | 12.1k | 5.4k (-55%) | 11.2k (-7%) | 14.0k (+16%) |
| | calls | 7 | 2 (-71%) | 5 (-29%) | 9 (+29%) |
| | time | 29.9 s | 17.3 s (-42%) | 27.6 s (-8%) | 46.2 s (+54%) |
| One-hop (6) | tokens / calls / time | 39k / 2.5 / 13.9 s | 35k / 2 / 12.2 s | 51k / 2.5 / 15.8 s | 67k / 5 / 21.0 s |
| Text (6) | tokens / calls / time | 49k / 4 / 16.9 s | 58k / 3.5 / 16.9 s | 78k / 3.5 / 17.5 s | 53k / 3.5 / 16.5 s |

Every arm answered every question correctly in every run. **Only st meets the targets, and fully only on multi-hop
questions**: there it meets all four (tokens -60%, context -55%, calls -71%, time -42%). Over all questions it meets
the context, calls and time targets and misses tokens by a point (-49%). On one-hop and text questions st is at
parity with grep, as in Phase 2. Neither IntelliJ nor jdtls meets any target. IntelliJ makes fewer calls but uses
more tokens. jdtls is the slowest and most expensive arm on every shape except text.

## Per question

Verdicts per repeat (C correct), then median calls / tokens / time.

| Q | Shape | grep | st | intellij | jdtls |
|---|---|---|---|---|---|
| F1 | multi, entry points through an interface | CCC 8 / 141k / 29 s | CCC 3 / 81k / 21 s | CCC 5 / 140k / 33 s | CCC 9 / 165k / 45 s |
| F2 | multi, path through beans to an HTTP client | CCC 7 / 87k / 24 s | CCC 2 / 41k / 16 s | CCC 7 / 130k / 34 s | CCC 7 / 117k / 35 s |
| F3 | multi, path from a controller to an HTTP client | CCC 7 / 119k / 32 s | CCC 7 / 213k / 35 s | CCC 8 / 166k / 38 s | CCC 10 / 201k / 49 s |
| F4 | multi, entry points through an interface | CCC 4 / 47k / 15 s | CCC 4 / 65k / 19 s | CCC 4 / 73k / 19 s | CCC 4 / 66k / 28 s |
| F5 | multi, signature-change impact | CCC 6 / 124k / 33 s | CCC 1 / 28k / 14 s | CCC 2 / 64k / 18 s | CCC 9 / 157k / 50 s |
| F6 | multi, stack trace to controllers | CCC 8 / 147k / 33 s | CCC 2 / 37k / 16 s | CCC 3 / 109k / 19 s | CCC 9 / 143k / 46 s |
| F7 | multi, path through a strategy list | CCC 7 / 77k / 25 s | CCC 2 / 40k / 17 s | CCC 5 / 117k / 28 s | CCC 13 / 185k / 54 s |
| F8 | one-hop, implementations and callers | CCC 4 / 60k / 22 s | CCC 2 / 36k / 14 s | CCC 3 / 57k / 20 s | CCC 7 / 112k / 35 s |
| F9 | one-hop, direct callers | CCC 2 / 31k / 11 s | CCC 2 / 35k / 11 s | CCC 2 / 49k / 12 s | CCC 3 / 48k / 14 s |
| F10 | text, cron property and file | CCC 4 / 62k / 20 s | CCC 5 / 88k / 23 s | CCC 5 / 101k / 22 s | CCC 5 / 78k / 22 s |
| F11 | text, error key | CCC 4 / 44k / 15 s | CCC 3 / 46k / 13 s | CCC 2 / 49k / 12 s | CCC 2 / 35k / 11 s |

## Where each tool wins and loses

- **st** wins every multi-hop shape where the agent can name the target: the impact question (F5, 1 call against
  6), the stack trace (F6), paths through beans when both ends are known (F2, F7), and entry points (F1). It loses
  F3, a path whose end the question describes but doesn't name ("the HTTP call that ..."). The agent walked
  `st callees --depth 4-5` from the start instead of `st path`, and spent 213k tokens against grep's 119k. On F4 and
  the text questions it is slightly above grep: it cross-checked with st on 3 of 6 text runs.
- **IntelliJ** is right as often as everyone else and uses its tool on 93% of structural runs. It wins on the
  impact and stack-trace questions (F5, F6: 2-3 calls). It loses on paths and entry points (F1, F2, F3, F7): 13 of
  its 33 `analyze_calls` results were larger than Claude Code accepts (up to 1 MB at depth 8-12), in 12 runs. The
  agent then read the saved file or fell back to grep. Its tokens are above grep on 7 of 11 questions.
- **jdtls** is right as often as everyone else and uses `LSP` on 93% of structural runs, but it never pays off. It
  answers one level per call: 74 LSP calls, 41 of them `incomingCalls`. It needs up to 13 calls on a multi-hop
  question and is the slowest arm on every structural question. 17 of its 74 LSP calls failed: `workspaceSymbol` was
  called with `.` as the file, and the tool requires a file path.
- **grep** stays the right tool for text questions, and for one-hop questions it is as cheap as any arm.

## Failure modes

- **Oversized results** (intellij): `analyze_calls` defaults to depth 5, and agents raised it to 8-12, so a call
  tree in this service runs to hundreds of kilobytes. Output over the limit is saved to a file instead, and the run
  continues with grep and Read.
- **Tool-call errors** (jdtls): `workspaceSymbol` needs a file path in Claude Code's LSP tool. The arm's note said
  that every operation except `workspaceSymbol` takes one, so the note caused these errors. This is a measurement
  flaw to fix in the note (see below); it added calls and time to the jdtls arm.
- **Per-session startup** (jdtls): the plugin starts jdtls in every agent session, at the first LSP call. jdtls
  question time was 46 s median in the 22 runs that started it and 14 s in the 11 that did not. The two groups
  differ in shape (the 11 are text, one-hop and two multi-hop runs that never called LSP), but on multi-hop
  questions alone the jdtls runs that started it took 46 s against grep's 30 s. The startup probe answered a symbol
  query 3.3 s after launch on the warm workspace, but the first real call waits longer (about 30 s in the spike).
- **No wrong answers from guessed types or stale results** were found in any arm. The spike's known IntelliJ fault
  (a Lombok getter resolved silently to a library class with the same simple name) had no question here that
  touched it.
- **No timeouts and no failed runs.**

## Lesson: an ambiguous question defeats every tool

On the dropped question[^1] every arm split between the two readings of its wording, st included: three runs
followed the literal two-hop reading and listed none of the expected tests (one each for grep, IntelliJ and jdtls),
seven listed more than the ground truth (all three st runs among them), and only two matched it (one grep run, one
jdtls run). No tool made the answers agree, because the disagreement was in the question, not in the code. Tool
choice can't make up for an unclear question. Before the runs, check each question's wording against its ground
truth for a second reading, in particular hop counts, "directly or through" and what counts as an entry point.

## Setup and maintenance cost

| Arm | Warm-up per project | Per-session startup | Disk / memory | Setup and upkeep |
|---|---|---|---|---|
| st | index `st index --full` 144 s wall (70 s indexing, rest Gradle), then incremental; server start 0.1 s | none (warm server) | 365 MB with the index | the fork: C++ build, Java indexer, delombok and the JDK/libjvm settings; the index must be rebuilt after indexer upgrades |
| intellij | 61 s to ready (IDE uptime at the last dumb-mode exit; command wall 83 s with checks) | none | the IDE system dir (with the jdtls workspace, 1.2 GB of tool state), 4 GB heap | an IntelliJ install (2025.2+); a headless second instance with its own config dir; readiness needs log parsing; every tool call must pass `projectPath`; the tool whitelist must be kept |
| jdtls | 75 s (import and full build; first answer at 67 s) | about 3 s probe, about 30 s at the first real call | the workspace (see intellij) | free; a plugin plus a PATH wrapper for Lombok and a workspace per checkout; writes `.project`, `.classpath`, `.settings` and `bin/` (with copied resources) into the tree; close to the plugin's 120 s startup timeout on a cold workspace |

## Recommendation

**Keep st, and make it the default for structural questions.** It is the only arm that meets the spec targets, and
it meets them where tracing matters, on multi-hop questions: 60% fewer tokens, 71% fewer calls and 42% less time
than grep, with the same correctness. It is also the cheapest arm in total (USD 3.18 against 4.17 for grep).

**Don't switch to IntelliJ MCP or jdtls for agents.** Both are as correct as st and grep here, and agents do adopt
them, but neither saves anything against grep: IntelliJ costs 21% more tokens and jdtls 57% more. jdtls pays its
startup in every session as well.

**Combine only where st is blind.** Phase 2's limits (records, annotation-processor sources, field writes) are
unchanged. IntelliJ is the natural fallback there, since it resolves generated code. That combination was not
measured and would need its own questions.

Next steps:

1. Fix the notes before any later use of these arms: `workspaceSymbol` also takes a file path (any file of the
   project), and IntelliJ's `analyze_calls` works better at `depth` 1-3, expanded with `treePath`. These arms were
   not rerun; the results above stand for the notes as written. That is a caveat for jdtls in particular: about a
   fifth of its LSP calls failed because of the note.
2. For st, teach the snippet one more idiom for paths whose end is described but not named:
   `st callees A --depth N | st filter --kind method --package ...`, or find the target with `st find` first, then
   `st path`.
3. Run the committed realworld trace set (`questions/realworld-trace.json`) with the same arms, for a public,
   reproducible second data point. This run covers one project and one model.

[^1]: The dropped question asked for tests "within at most three calls (the test calls A, A calls B, B contains the
    line)". The parenthesis describes two hops, while its ground truth needs three, so no arm could be scored fairly.
    Its 12 runs (3 per arm) are left out of every number above. As run: grep wrong, correct, partial; st partial,
    partial, partial; intellij wrong, partial, partial; jdtls correct, wrong, partial. The wrong runs followed the
    two-hop reading and listed none of the 9 tests. The partial runs had all 9 and added the tests of the next caller
    up. A wording that matches the ground truth would be "the test calls A, A calls B, and B calls C, the method that
    contains the line". The question was not rerun.
