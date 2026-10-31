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
   questions it suits) and grep where grep fits, and what stops it?

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

## Remaining issues and recommended fixes (ranked)

1. **Say when an answer can be trusted (README).** Add one sentence to the Incomplete answers paragraph: no
   `incomplete:` line means the index has every link the answer depends on, so re-reading source is only needed for
   what the index never holds (text, config, switch arms over records, the Lombok builder relation). This targets the
   largest cost in the unprompted run, which was confirmation reads. Cheapest fix, largest expected effect.
2. **Prefer the type over its constructors (st).** When a bare name matches a type and that type's own constructors,
   resolve it to the type for `impls`, `supers`, `deps`, `outline` and `impact`. Keep exit 2 for real ambiguity. This
   removed one call in U3 and would remove one in any class-level query on a Lombok code base.
3. **Name the project and the error owners (st).**
   - Print the project name on the stderr count line (`find: 1 [<project>]`), so a wrong-directory query is visible.
   - Make the `incomplete:` line, or `st status <symbol>`, name the symbols the index errors belong to, with a test or
     main marker. The agent could then dismiss the H5 warning, which came from a test base class, instead of
     treating the answer as a lower bound.
4. **Explain record misses (st).** On `symbol not found`, if the name's type matches an indexed file
   (`<Type>.java`) that defines no symbol of that name, say
   `st: <Type> is declared in <file> but not indexed (Java record? see README Known limits)`. H7 failed loudly but
   without a reason. This makes the message self-explaining for agents that skipped the README.
5. **Connect Lombok builder setters to their field (st, or README).** `refs`/`impact` of a `@Builder` field should
   include the builder setter of the same name, or the README should say to also query `<Type>Builder.<field>(...)`.
   The H3 answer was short without a warning because of this.
6. **Allow a parameter prefix (st, optional).** Accept `(LocalDate, ...)` or a unique leading-parameter match for an
   overload. Exit 3 with the overload list already makes a wrong guess cost only one call, so this ranks low.
7. **Package walks include tests (st, optional).** `deps`/`impact` of a package also walk test classes in the same
   package, which inflated the H5 link counts and caused its warning. A `--no-tests` switch, or a test/main split in
   `deps` rows, would let main-code questions stay main-only. Today `| st filter --not --file '*Test*'` works only on
   symbol rows, not on package rows.
