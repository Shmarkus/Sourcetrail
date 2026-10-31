# st agent evaluations: holdout and unprompted use

Date: 2026-10-31. Branch: `feat/st-trust`. Binary: `build/Release/app/st`.

## Purpose

The benchmark (`2026-10-31-st-benchmark-results.md`) measured `st` on questions that were also used to find and fix
problems, so it can overstate how well `st` does. This document reports two further runs that were not used for
tuning:

1. **Holdout.** Seven new navigation questions, answered by an agent told to use `st`. Does `st` stay correct and cheap
   on questions nobody tuned for, and does it warn when an answer touches a gap in the index?
2. **Unprompted.** Eight tasks phrased the way a developer asks a coding agent, with no tool named. The agent got only a
   one-line project note saying `st` exists. Does it choose `st` where `st` fits (spec target: on at least 80% of the
   questions it suits) and grep where grep fits, and what stops it? A second unprompted run (run 2) repeats the eight
   tasks with a CLAUDE.md snippet as the project note, to see whether it cuts unnecessary source re-reads.

## Method

- **Two codebases.** Project A is a multi-module Spring Boot/Gradle service with heavy Lombok use (693 indexed files,
  141 index errors in 22 files, all from JPA metamodel and MapStruct classes generated at build time). Project B is a
  small Java 21 batch library (111 files) that uses records and sealed interfaces, which the bundled Java indexer does not
  index as types. Class, method and package names of both projects are left out of this document.
- **Ground truth before st.** Both question sets and their answers were built with `grep`, `find`, `sed` and file reads
  only, before `st` was run on them. The cost of building each answer (tool calls and output characters) is the grep
  baseline. No target of the earlier benchmark is reused.
- **Holdout never tuned on.** No `st` change was made because of a holdout result.
- **Unprompted agent saw only the note.** Its prompt was the task list plus the project's one-line note pointing at the
  README section on `st`. It chose its tools freely.
- **Scoring.** Each answer was compared with the ground truth. Doubtful answers were checked here by re-running `st`
  and reading the source. Every miss is assigned to `st` or to the agent.
- **Counting.** A tool call is one logical command (`st`, `rg`, `sed`, `cat`, `awk`), a pipeline counting as one; this
  is the unit of the grep baseline. The unprompted agent often put several commands into one shell call, so both numbers
  are given for it. Characters are the output the agent received, as the agents estimated them.

## Holdout run

| # | Question shape | Result | st behaviour | st calls / chars | grep calls / chars |
|---|---|---|---|---|---|
| H1 | Callers of an adapter method that is reached only through its interface | correct | Both callers, each marked `via <interface method>`; no warning, none needed | 1 / 1.4k | 4 / 9k |
| H2 | Callers of a Lombok class-level `@Getter` getter; same-named getters exist on other classes | correct | The 4 hand-written callers plus the generated `equals`/`hashCode`/`toString` (accepted by the ground truth); look-alikes excluded | 1 / 2.3k | 4 / 12k |
| H3 | Impact of changing the type of a field of a nested Lombok `@Value @Builder` class, one level up | **partial** (agent) | `refs` of the field gave only the generated constructor and getter. The agent stopped there and did not query their callers | 2 / 2.5k | 3 / 8k |
| H4 | Stack frame `File.java:N` on the continuation line of a lambda, with a same-named overload below | correct | `st at` gave the enclosing overload, not the one the lambda calls | 1 / 0.5k | 3 / 7k |
| H5 | Outgoing project-package dependencies of a 26-file package tree | correct (superset) | 18 packages vs 16 in the ground truth; the 2 extra are real (see below). Warned `incomplete: 4 index errors` | 1 / 3.2k | 5 / 10k |
| H6 | `@Test` methods reaching a service method within 4 call levels | correct | `callers --depth 4 \| filter --annotated Test` gave exactly the 35 tests; the HTTP-only integration tests were found by rg and described as not statically reachable | 5 / 6k | 8 / 18k |
| H7 | Callers of a record's implicit accessor (known blind spot) | correct, via rg | `symbol not found`, exit 3. The agent fell back to rg and type-checked the receivers; one look-alike was left "uncertain" rather than excluded | 4 / 2.5k | 4 / 9k |
| | **Total** | **6 correct, 1 partial** | | **15 / ~19k** | **31 / ~73k** |

`st` used 52% fewer calls and 74% fewer characters than the grep baseline.

**H3 is an agent miss, not an `st` miss.** Checked here: `st callers <getter> --depth 2` returns both direct users
and the method one level up, and `st callers <builder setter>(Duration)` returns the builder call site and its caller.
Two more calls would have given the full answer. One `st` weakness shows here: `refs` and `impact` of a Lombok
field do not reach the builder setter, because the builder has its own field of the same name. `impact --depth 3` reaches
the builder call site only through `build()`. The agent saw the builder setter in `find` output and still did not
query it.

**H5's two extra packages are right.** Both are used without an import: one through a chain of getter calls, the other through a lambda parameter typed by a repository's return type. The
ground truth was built from import statements and cannot see such uses. Every one of the 18 rows has its example site in
main code.

### Warnings in the holdout

- **The one `st: incomplete:` line (H5) was produced by the documented rule but did not matter for this question.**
  The package walk includes the two test classes that share the package name in the test source set. Both extend a test
  base class in another package whose imports of MapStruct-generated mapper classes are unresolved. The 4 errors
  belong to that base class, so the warning fires. They cannot remove a dependency of the main-code package the question
  was about. The agent treated the list as a lower bound, which was safe but unnecessary. It could not have checked,
  because `st status` prints only index-wide totals and does not say which symbols the errors belong to.
- **The record blind spot (H7) failed loudly.** It exited 3 with `symbol not found`. The agent knew why only from the
  README's Known limits. The message itself does not say that the type is a record and is not indexed. An agent that
  had not read the README could read it as "no such method".
- **Silent-gap check.** No holdout answer was silently incomplete. H1, H2, H4 and H6 match the ground truth exactly and
  were rightly silent. Additional probes on project B, where calls made inside record bodies are attributed to no
  symbol, all warned:
  - `callers` of a method called only from a record body: `callers: 0` plus `incomplete: 1 links to unresolved symbols`.
  - `callers` of two stage methods whose only production caller is a record method: listed every other caller, and
    warned.
  - `impact` of the sealed interface whose subtypes are records: warned, `5 links to unresolved symbols`.
  The one place where an answer was short without a warning is the Lombok builder relation in H3. That is a
  modelling gap, not an index error, so the warning cannot catch it.

## Unprompted run

The agent read the README section on `st` once (4 setup calls, ~11k chars, not in the table).

| # | Kind | Task shape | Tools chosen | Fit? | Result | Shell calls / commands / chars | grep calls / chars |
|---|---|---|---|---|---|---|---|
| U1 | structural | Add a fifth permitted subtype to a sealed interface: what stops compiling? | `st find`, `st impact`; grep/sed for switches and record cases | yes (st) | correct; one test `instanceof` site not named (its grep covered only `src/main`) | 5 / 11 / 21k | 3 / 6k |
| U2 | structural | Startup chain to an adapter method reached through a port interface | `st callers --depth 4`; sed to confirm the wiring | yes | correct | 2 / 7 / 8.5k | 3 / 7k |
| U3 | structural | Implementations of an event-handler interface and where each is used | `st impls`, `st callers`; sed for qualifiers and stream wiring | yes | correct and complete, including the test-only lambda and the callers of the publisher | 3 / 12 / 10k | 3 / 7k |
| U4 | structural | Rename impact of a `@FunctionalInterface` method | `st impact`, `st impls`; grep for config and reflection | yes | correct, plus the transitive users | 2 / 5 / 5k | 2 / 5k |
| U5 | structural | Callers of one overload of a static utility only | `st callers` (overload-exact), grep cross-check | yes | correct, exact overload split | 4 / 7 / 5k | 3 / 7k |
| U6 | stack trace | Frame `File.java:N` inside a lambda: what runs there, who calls it | `st at`, `st outline`, `st callers --depth 3`; sed | yes | correct | 2 / 5 / 9k | 2 / 9k |
| U7 | text | Where a base URL in notifications comes from (config property, templates) | grep, sed, cat only | yes (grep) | correct | 3 / 10 / 5k | 3 / 5k |
| U8 | text | Where a log message comes from and when it fires | grep, sed only | yes (grep) | correct | 2 / 3 / 3k | 2 / 3k |
| | | **Total** | | **8 / 8 fitting choices** | **8 correct** | **23 / ~60 / ~66k** | **21 / ~49k** |

**Adoption.** `st` was used on 6 of the 6 tasks it suits (5 structural, 1 stack trace), which is 100% against the 80%
target. Neither text task used `st`, which is also the right choice. The run log's own summary says "`st` was used in
8 of 8 requests". Its per-task notes contradict that (U7 and U8 are grep only), and the per-task notes are correct.

**Cost.** Unprompted use did **not** save effort: 23 shell calls (~60 logical commands) and ~66k characters, against
21 calls and ~49k for grep. The holdout agent was told to use `st` and spent half the calls and a quarter of the
characters of grep. The unprompted agent used `st` as a first step and then read or grepped the source to confirm
almost every answer (21 `st` commands, ~39 grep/sed/cat/awk commands). Part of that reading was necessary:
- switch arms and pattern cases over record types (U1) are not in the index,
- bean qualifiers and stream bindings (U3) are text,
- it checked for reflection and config uses before a rename (U4).
The rest was confirmation that the README does not tell the agent it can skip. U1 alone cost 21k characters, against
6k for grep, because of the false starts listed below.

### Friction seen in the unprompted run

1. **Wrong project, silently.** The first U1 command ran `st find` in project A's directory for a question about
   project B. It got one unrelated hit with no hint which project answered. `st` picks the project from the current
   directory, and nothing in its output names it.
2. **Parameter guessing (U5).** The agent guessed the overload as `(LocalDate)`, but the real signature has a second
   parameter. `st` exited 3 and listed both overloads, so the cost was one extra call. Checked here: simple type names
   such as `(LocalDate, Clock)` resolve, so only the parameter count was wrong.
3. **Class name ambiguous with its own constructors (U3).** `st impls <EventClass>` exited 2 because the bare name
   matches the class and its Lombok-generated constructors. The same happens here for `st deps <ClassName>` on every
   class tried with an explicit or generated constructor. For `impls`, `supers`, `deps` and `outline`, a constructor is
   never what the user means.
4. **zsh glob errors (U1, twice).** `grep --include=*.java` unquoted fails under zsh with `no matches found`. This is a
   grep problem, not an `st` problem, but it cost calls in the same session, and a README example with quoted globs
   would help.
5. **No signal that `st` alone is enough.** Nothing tells the agent when an answer is complete, so it re-verified
   answers that had no `incomplete:` line. The README explains the `incomplete:` line but does not say that its absence
   means the answer can be trusted.

## Run 2: with the CLAUDE.md snippet

A fresh agent redid the eight unprompted tasks. Its only project note was the snippet in
`docs/agents/st-claude-md-snippet.md`. The snippet does three things:
- It tells the agent to use `st` for structure and grep for text, config, strings and log messages.
- It says to trust an `st` answer without re-reading the source, unless stderr shows `st: incomplete:` or the question
  touches a listed blind spot (records, Lombok `@Builder` setters).
- It points to the README's new "For agents" block.

Between the two runs, `st` gained the project name on the count line (`callers: 4 (project <name>)`). A bare class name
now resolves to the class instead of being ambiguous with its constructors. Both changes were confirmed here.

### Correctness

All 8 answers are correct. Details:
- **U1** now also names the test `instanceof` site that run 1 missed.
- **U8** attributes the "already done" return to the planner's public method. The return is actually in a private helper
  that the public method calls, so this is a naming slip, not a wrong answer.
- **U5** shows a new `st` problem. One caller, a test helper, was printed with the path of its delombok copy under
  `sourcetrail_dependencies/.../delombok/test/` instead of its source path. The agent still identified it correctly.
  - Cause, checked here: two modules each have a test helper with the same package and file name. Their test source
    sets are delomboked into one shared `test` tree, so the two copies land on the same relative path. The manifest
    lists both, but only one file survives on disk. `st` cannot map that path back to one original file.
  - The other copy is therefore not in the index at all, and no warning reports it (a silent gap). It did not affect
    any answer in this run.

### Cost and tool choice

| # | Kind | Run 1 (commands / chars) | Run 2 (commands / chars) | grep (calls / chars) | Run 2 tool choice |
|---|---|---|---|---|---|
| U1 | structural | 11 / 21k | 11 / 14k (3 of them setup) | 3 / 6k | `st refs`, then grep/sed (records) |
| U2 | structural | 7 / 8.5k | 10 / 12k | 3 / 7k | `st` callers/impls/refs/outline, 2 sed, 1 grep |
| U3 | structural | 12 / 10k | 11 / 8k | 3 / 7k | `st` impls/callers/refs, 5 sed, 1 grep (Spring wiring) |
| U4 | structural | 5 / 5k | **4 / 4k** | 2 / 5k | `st` only |
| U5 | structural | 7 / 5k | **4 / 5k** | 3 / 7k | `st` only |
| U6 | stack trace | 5 / 9k | 9 / 9k | 2 / 9k | `st at \| st callers`, sed for the statement |
| U7 | text | 10 / 5k | 6 / 4k | 3 / 5k | grep, cat, sed; one `st refs` cross-check |
| U8 | text | 3 / 3k | 3 / 5k | 2 / 3k | grep, then `st callers` for the caller |
| | **Total** | **60 / ~66k** (+4 / 11k setup) | **58 / ~61k** (setup included) | **21 / ~49k** | |

Counting notes:
- Run 2's chars are the sum of its per-task estimates. Its log header says "about 45k", which does not match its own
  per-task figures, so the per-task sum is used here.
- Including setup, run 1 cost 64 commands and ~77k characters; run 2 cost 58 commands and ~61k characters.
- `st` commands: 21 in run 1, 28 in run 2. Other commands: ~39 in run 1, ~30 in run 2.

**Tool choice is still right on every task.** `st` was the primary tool on all 6 structural and stack-trace tasks (100%
against the 80% target). grep was the primary tool on both text tasks. In those, `st` appeared only as a follow-up: a
`refs` cross-check in U7, and finding the planner's caller in U8.

**Cost improved about 20% but is still above grep.** On the pure structure tasks (U4, U5), the snippet worked as
intended: `st` only, no re-read, and cheaper than grep. Overall, the agent spent ~2.8 times grep's commands and ~1.25
times its characters.

### Were the source re-reads justified?

Run 2 re-read source after an `st` answer on 4 of the 6 `st` tasks (U1, U2, U3, U6). Run 1 did so on all 6. In U8
grep came first, so its reads are not re-reads of an `st` answer.

| # | Re-read | Verdict |
|---|---|---|
| U1 | grep for switch arms and `instanceof`; read the sealed interface, the routing switch, the stage switch, two enums | **Justified.** The permitted subtypes are records, a listed blind spot, and `st refs` warned `incomplete: 1 links to unresolved symbols`. |
| U2 | Read the runner's `run` method and grep for `main(` | **Justified.** The framework calls the runner (`ApplicationRunner`) and the adapter is built with `new` inside `run`. Neither is a call edge. |
| U2 | Read the guard method; plus a no-op `st ... \| head -0` and repeated `st refs`/`impls` after `callers --depth 6` already showed the chain with the `via <interface>` hop | **Unnecessary** (about 4 commands). |
| U3 | Five reads for bean names, the qualifier, the stream binding and the test lambda bean | **Justified.** Spring wiring by bean name and stream binding are not in the index. |
| U6 | Read line 95 | **Justified.** The question asks what runs on that line, and `st at` names the enclosing method, not the statement. |
| U6 | `find`, a second `sed`, a second `st at`, a second `callers --depth 3` piped through `grep -v Test` | **Unnecessary** (about 4 commands). Part of the cause is in `st`: `callers --depth` has no depth column, so the agent re-ran it to tell direct callers from transitive ones. |
| U8 | Read the planner for the condition | **Justified.** The question asks when the log line fires, which needs the statement-level logic. |

About 8 of 58 commands were unnecessary. The other re-reads were for things the index does not hold. In run 1,
unnecessary checks included:
- a grep cross-check after an overload-exact `st callers` (U5),
- confirmation reads of a chain `st` had already given (U2).
The snippet removed those.

### Warnings in run 2

Two `st: incomplete:` lines appeared, and both were justified:
- **U1 `refs`:** a link to a record type.
- **U2 `callers`:** 1 index error in the runner's `run` method. A type it constructs belongs to a package missing from
  this copy of project B.
Neither changed an answer, and the agent's answers did not depend on the missing links.

### Conclusions after run 2

- **Adoption is solved.** Both unprompted runs pick `st` for 6 of 6 suited tasks and grep for text.
- **The trust sentence works where it applies.** Pure structure questions are now answered with `st` alone, cheaper
  than grep.
- **The remaining overhead is mostly legitimate.** Records and Spring wiring are outside the index, and some
  questions need the statement text.
- **What remains fixable** is about 8 redundant commands per run (repeated queries, flat `--depth` output) and the long
  setup in U1.
- **The gap to grep is structural.** The unprompted tasks are deliberately mixed. On them, `st` cannot match the told
  agent's 52% call saving from the holdout run, because a third of the tasks need source anyway.

## Remaining issues and recommended fixes (ranked)

Status after run 2: items marked *done* were confirmed in the current binary or docs.

1. ~~**Say when an answer can be trusted (README).**~~ *Done* (snippet and README "For agents"). Effect confirmed: no
   re-reads on U4/U5, re-reads down from 6 to 4 tasks.
2. ~~**Prefer the type over its constructors (st).**~~ *Done*: a bare class name now resolves to the class.
3. **Name the project and the error owners (st).** The project name on the count line is *done*. Still open: the
   `incomplete:` line, or `st status <symbol>`, should name the symbols the index errors belong to, with a test or main
   marker. That would let an agent dismiss the H5 warning, which came from a test base class.
4. **Add a depth column to `callers`/`callees --depth` (st, new).** `impact` already prints `depth <n>`. In U6 the
   flat list cost a re-run and a source read to split direct callers from transitive ones.
5. ~~**Fix delombok output collisions across modules (indexer, new).**~~ *Done* (Phase 2.5 Task 8). Same-named files with the same package in two
   modules' test (or main) source sets overwrite each other in the shared delombok tree. One copy silently drops out of
   the index, and the other is reported under its delombok path.
   - Fix: each source root is delomboked into its own `delombok/<set>/<root>` tree, a file's own
     root comes first on its class path, and `st` lists a symbol at every file that declares it.
   - Remains: both copies are one merged symbol, and a third module resolves the class to the first copy on the class
     path (module-aware resolution, parked as a follow-up).
6. **Explain record misses (st).** Still open. `symbol not found` on a record accessor gives no reason. Suggested
   message: `st: <Type> is declared in <file> but not indexed (Java record? see README Known limits)`.
7. **Connect Lombok builder setters to their field (st).** Still open in `st`. The snippet now lists `@Builder` setters
   as a blind spot, which covers agents that read it.
8. **Allow a parameter prefix (st, optional).** Exit 3 with the overload list cost one extra call in both unprompted
   runs (U5). This is low priority.
9. **Package walks include tests (st, optional).** `deps`/`impact` of a package also walk the test classes in that
   package. A `--no-tests` switch, or a test/main split in `deps` rows, would keep main-code questions main-only.
