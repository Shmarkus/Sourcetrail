# st benchmark results

Date: 2026-10-31. Branch: `feat/st-architecture`. Binary: `build/Release/app/st`.

## Purpose

`st` is the agent-facing command line over the Sourcetrail index. The spec says it is worth having if an agent answering code-navigation questions with `st` needs:

- at least 50% fewer tool calls than with grep and file reads,
- at least 50% fewer output tokens (measured here as output characters),
- with correctness no worse than grep,
- and each query runs in under 1 s (`path --depth 6` in under 2 s).

This document reports whether `st` meets those targets on two real Java codebases.

## Method

1. **Question set and ground truth, built without st.** 14 questions cover the 12 query shapes in the spec: overload callers, implementations, controller-to-repository path, tests reaching a line, signature-change impact, Lombok getter callers, stack-frame lookup, outline, package dependencies, symbols in a file, annotation filter, and two plain text searches. They were asked against two codebases: *shop*, a Spring/Lombok multi-module Gradle service (13 questions), and *batch*, a small batch framework (1 question). The ground truth was built with `grep`, `find`, `sed` and file reads only, before `st` was used. Its cost (calls and output characters) is the grep baseline.
2. **Run 1.** A separate agent answered all 14 questions with `st`, using rg only where `st` has nothing to offer. Its answers were scored against the ground truth. Every miss was traced to a root cause.
3. **Fixes.** The st and indexer fixes listed below were implemented and the shop project was re-indexed.
4. **Run 2.** A fresh agent answered the same questions with the fixed binary. Its answers were scored the same way, and doubtful answers were checked by re-running `st` and reading the source.

Counting rules:

- A tool call is one logical command: each `st`, `rg`, `sed` or `cat` invocation, with a pipeline counted as one. This is the same unit as the grep baseline. (The run 2 agent put several commands into one shell call; they are counted separately.)
- Characters are the output the agent received.

Scoring corrections to the ground truth: in both runs `st` was right where grep was not.

- **Q2:** add the anonymous subclass of `InboundPaymentEventConsumer` in `InboundPaymentEventConsumerUnitTest.java:61-74`.
- **Q5:** add `DefineOrder{,V3}EventConsumer.accept`, reached through the method references in `StreamConfiguration.defineOrder{,V3}MessageConsumer()` (`:50`, `:55`).
- **Q7a:** the method signature is at line 177.
- **Q4:** the integration-test source set is indexed since the fixes, so the IntTest counts as expected.

With these corrections, the grep baseline itself scores 12 of 14.

## Results per question

Verdicts: **C** correct, **P** partial, **W** wrong. Calls and characters are given as grep / run 1 / run 2.

| Q | Shape | Run 1 | Run 2 | Calls | Chars (k) | Run 2 notes |
|---|---|---|---|---|---|---|
| 1 | overload callers | C | C | 5 / 2 / 2 | 12 / 2.2 / 1.1 | First guess of the parameter FQNs was wrong, giving exit 3 with no hint (since Task 14 the exit 3 lists the overloads). Simple type names, as written in the question, then worked. |
| 2 | implementations | C | C | 2 / 1 / 1 | 1.2 / 1.0 / 0.9 | All 4, including both anonymous classes. |
| 3 | path | C | C | 4 / 1 / 1 | 25 / 1.6 / 1.2 | Correct 4-hop path. Two call sites are given at the first line of the statement (141, 182) instead of the call line (143, 183); since Task 14 they print 143 and 183. |
| 4 | tests reaching a line | P | C | 7 / 3 / 2 | 40 / 4.8 / 3.4 | 9/9, including the IntTest. `at …:25` now works. |
| 5 | signature impact | P | C | 6 / 3 / 3 | 20 / 8.2 / 16.6 | Direct, transitive and hop depth are all correct. Needed a second, filtered `impact`, because 185 of the 203 rows are tests. |
| 6 | Lombok getter callers | C | C | 6 / 1 / 1 | 18 / 2.7 / 2.6 | 11/11, no delombok paths. Three lines are statement starts: 189 for 190, 199 for 200, 189 for 194; since Task 14 they print 190, 200 and 194. |
| 7 | enclosing symbol | C (fallback) | C | 2 / 2 / 2 | 3 / 11.9 / 0.6 | Both `at` calls answer directly. The agent misquoted the start line as "~174"; `st` prints 177. |
| 8 | outline | C | C | 1 / 1 / 1 | 0.9 / 1.4 / 10.2 | All 30 members, with source. Run 1 piped through `grep public`. |
| 9 | package deps | P | C | 4 / 1 / 1 | 7 / 0.6 / 0.8 | All 6 target packages, and no dependency on rest. |
| 10 | symbols in file | C | C | 1 / 1 / 1 | 4 / 6.5 / 7.0 | All symbols. The real nested `@Value` class and its 2 fields print "(generated)" with no line. 19 of the 28 rows are Lombok members. |
| 11 | annotation filter | C | C | 3 / 1 / 1 | 8 / 3.2 / 0.8 | 3/3, with both `search` overloads. |
| 12 | config key | C (rg) | C (rg) | 2 / 1 / 1 | 1.5 / 0.8 / 0.6 | Text search, by design. |
| 13 | string literal | C (rg) | C (rg) | 3 / 2 / 1 | 4 / 1.1 / 0.5 | Text search, by design. |
| 14 | static-import callers | P (severe) | C | 2 / 2 / 1 | 9 / 3.5 / 1.3 | 8/8 callers, including the 3 static field initializers. |

**Tally:** run 1 had 10 correct and 4 partial. Run 2 has **14 correct, 0 partial, 0 wrong**.

## Medians against the targets

| Metric | grep | Run 1 | Run 2 | Target | Run 2 met? |
|---|---|---|---|---|---|
| Median calls per question | 3 | 1 | 1 | — | — |
| Median of per-question call ratios (st/grep) | — | 0.50 | **0.45** | ≤ 0.50 | Yes |
| Total calls | 48 | 22 (−54%) | **19 (−60%)** | — | — |
| Median of per-question char ratios (st/grep) | — | 0.39 | **0.14** | ≤ 0.50 | Yes |
| Total chars | ~154k | ~49k (−68%) | **~47k (−69%)** | — | — |
| Correct answers | 12/14 | 10/14 | **14/14** | no worse than grep | Yes (better) |
| Query time | — | all under 1 s | all under 1 s; `path` (depth 6) 0.33 s; worst pipeline 0.67 s | under 1 s; path under 2 s | Yes |
| Median command time per question | not recorded | 0.30 s | 0.34 s | ≥ 30% faster than grep | Not measured |
| Total command time | not recorded | ~7.2 s | ~5.8 s | — | — |

Time per question is the wall time of the question's commands only, as each agent recorded it (run 1: the summary table of its answers; run 2: the `Time:` line of each answer). It leaves out the model's own latency, so it is not the start-to-answer time the plan asked for, and the grep baseline recorded no times. The "≥ 30% faster" target is therefore not measured. Per-question times range from 0.01 s (rg) to 1.9 s (run 1, Q5: three commands) and 1.05 s (run 2, Q5: two commands).

In run 2, st costs as many calls as grep on Q7, Q8 and Q10. Its output is larger than grep's on Q8 (10.2k vs 0.9k) and Q10 (7.0k vs 4k). Q5 is close to grep (16.6k vs 20k). These three questions account for 72% of run 2's characters.

## What was fixed between runs

| Fix | Root cause | Questions |
|---|---|---|
| Lombok line mapping: byte-identical delombok copies map line for line, and a statement delombok joined onto one line maps to its unique run of original lines. Rows that stay generated print the original path, and `--no-generated` drops them. | Delombok reflows multi-line statements onto one line, so exact-line alignment found no counterpart. The repeated-line guards also refused lines even in unchanged copies. 26% of result rows printed 200-char delombok paths. | Q1, Q3, Q4, Q5, Q6, Q7, Q11 |
| `at` fallback: an unmapped line is answered when every line between its mapped neighbours gives the same symbol. | All four `at` calls in run 1 were refused with no hint. | Q4, Q5, Q7 |
| `callers` keeps field-initializer callers. | `walkCalls` erased every reached caller that was not a method or function. | Q14 |
| `deps` collects sub-packages by name prefix. | The Java indexer omits many package-to-sub-package member edges (79 of 219 packages). | Q9 |
| Parameter matching on simple type names, element by element. | Keys matched only the whole parameter list, with fully qualified types and exact spacing. | Q1 |
| `impact` prints the call depth of each row. | Direct and transitive callers were not separated. | Q5 |
| All Gradle Java source sets are indexed (including integration-test and contractTest) when tests are included. | `init.gradle` exported only `main` and `test`, which left 55 files out of the index. | Q4 |

## Remaining problems in run 2

Task 14 (commit 820cca40) fixed problems 1 and 5 and the ordering part of problem 2, and they were checked on the shop index; problems 3, 4, 6 and 7 stay open. (The plan's Task 14 numbers these differently: its "1, 3, 5" are problems 2, 1 and 5 here.)

- **Problem 1, fixed.** A parameter list that matches no overload still exits 3, but stderr now lists the overloads in the ambiguity format. The run 2 guess (`…sendPayments(java.util.UUID, …payment.PaymentPlan, java.util.Collection)`) lists both `sendPayments` overloads with their keys and locations (`PaymentService.java:45`, `:59`). The exit stays 3, not the 2 recommended below.
- **Problem 2, ordering fixed.** `--limit` now keeps the lowest depths. `impact PaymentService.alterPayments` at the default limit keeps all 6 direct callers (depth 1), where run 2 showed 3 of them; the 50 kept rows cover depths 1-5. The README names the `| st filter --not --file '*Test*'` pipe. The per-depth main/test count and a `--no-tests` option were not added.
- **Problem 5, fixed.** A location inside a joined statement maps to the original line holding its column. The Q6 caller in `PaymentV2Service` is now printed at `:194` (was `:189`), the line with `order.getPaymentPlan()`. The Q3 path hops are `:143` and `:183` (were `:141` and `:182`). All of the more than 30 mapped sites checked hold their call.

| # | Problem | Root cause | Recommended fix | Type |
|---|---|---|---|---|
| 1 | **Fixed in Task 14.** **Q1:** a wrong guess of the parameter types gives `symbol not found`, exit 3, with no candidates. The agent guessed `service.payment.PaymentPlan`; the class is in `definition`. | `resolveSymbol` reports not-found as soon as no parameter list matches. It does not fall back to the name. | When the name without parameters resolves, list its overloads in the same format as the ambiguity message (exit 2), so that the next call can copy a key. | st fix |
| 2 | **Ordering fixed in Task 14; test counts and `--no-tests` open.** **Q5:** `impact` output is dominated by tests. 185 of 203 rows are tests (153 at depth 5). The default `--limit 50` view is sorted by file, not by depth, and it shows only 3 of the 6 direct callers. The agent needed a second call, and the answer cost 16.6k chars. `impact … \| st filter --not --file '*Test*'` would have given the 21 main rows in 4.9k chars, but the help does not mention it. | Rows are ordered by file and package, and truncated without regard to depth. There is no main-only mode. | Sort `impact` rows by depth, then by key, so that truncation drops the deepest rows first. Print a per-depth main/test count in the header. Add `--no-tests` (an alias for the file filter), or name the filter pipe in `impact --help` and the README. | st fix (+ usage) |
| 3 | **Q8:** `outline` has no visibility filter. A question about 5 public methods returned all 30 members with their source (10.2k chars; 7.4k with `--no-source`; 1.9k when filtered with `grep '\tpublic'`). `--kind` accepts only a bare kind, so `filter --kind 'public method'` is rejected. | Visibility is folded into the kind column but cannot be filtered on. | Add `--access public\|protected\|private` to `outline` and `filter`, or accept `--kind 'public method'`. Also print a hint to use `--no-source` when output exceeds a size threshold. | st fix (+ usage: `--no-source`) |
| 4 | **Q10:** the real nested class `CatalogAttachmentDto` (line 98) and its fields `id` and `fileName` (99-100) print "(generated)" with no line. `--no-generated` drops them, so it cuts the output to 2.2k chars but loses real symbols. | Lombok `@Value` rewrites the declarations: `public static class X {` becomes `public static final class X {`, and `String id;` becomes `private final String id;`. The lines are no longer equal, and the join-aware step only handles joins. | In the gaps between alignment anchors, compare lines with Java modifiers removed (`public`, `private`, `protected`, `static`, `final`), keeping the rule that the match must be unique in its gap. Until that is in place, `--no-generated` should keep symbols whose declaration name occurs in the original file. | st fix |
| 5 | **Fixed in Task 14.** **Q3, Q6:** a call site inside a joined multi-line statement is reported at the statement's first line, up to 5 lines away from the call (`PaymentV2Service.java:189` for `:194`). | Join-aware mapping maps the whole joined line back to the first line of its original run. | Use the reference column in the delombok line to find the original line inside the run. The whitespace-free concatenation already gives the offset of each original line. | st fix |
| 6 | **Q5:** 2 rows still print "(generated)", both call sites `defineOrderEventConsumer.accept(definition);` in `DefineOrderEventConsumerIntTest`. | `@SneakyThrows` wraps the method body in `try {}`, so the copy is not byte-identical. The call line occurs several times in the file, so the file-wide uniqueness guard refuses it. | Apply the uniqueness rule per gap between LCS anchors rather than per file. | st fix |
| 7 | **Q4:** `filter --annotated Test` also keeps methods whose *enclosing type* is annotated. Here that made no difference. | The behaviour is documented, but it is too broad for "@Test methods". | Add `--annotated-self` (or `--direct`) to match only the symbol's own annotations. | st fix (minor) |

## Known limits (not fixed)

- **Java records are not indexed** by the bundled Java indexer. Records and their components are missing from every query.
- **Sources generated by annotation processors are not indexed**, for example JPA metamodel classes (`Catalog_`, `Order_`) and MapStruct mapper implementations. On shop these unresolved types cause the 141 index errors. Calls made from generated code, such as MapStruct mappers calling getters, are invisible (see the Q6 ground-truth caveat).
- **Field reads and writes are not distinguished.** The index records a field access as a usage, so "who writes this field" needs `refs` plus reading the code.
- **Text search stays with rg.** Config keys, string literals and SQL (Q12, Q13) are outside the index, by design.
- **Calls through reflection or the framework**, such as Spring Cloud Stream invoking the `Function` beans, or HTTP calls in MockMvc tests, have no static edge. Method references are followed.
- **Static reachability is not coverage.** In Q4, tests that use mocks reach the line through interface dispatch but never execute it.
- **Missing package-to-sub-package member edges** in the Java indexer are worked around in `deps` but remain in the data.

## Recommendations

1. Fix item 3 and the rest of item 2. Q5 and Q8 are the only questions where `st` output approaches or exceeds grep. A test exclusion for `impact` and an access filter for `outline` would bring both well under the target. (`impact` sorting by depth and the overload hint of item 1 were done in Task 14.)
2. Fix items 4 and 6 together in the Lombok alignment. Match lines with modifiers removed and apply the uniqueness rule per gap (the exact line by column was done in Task 14). After that, only true Lombok members should print "(generated)", and `--no-generated` becomes safe to recommend by default for `symbols-in` and `outline`.
3. Next time, record start-to-answer time per question for grep and st alike, so the speed target can be judged.
4. In the README's agent section, document the idioms that worked in run 2:
   - `callers … | st filter --annotated …`
   - `impact … | st filter --not --file '*Test*'`
   - `--no-source` for listing commands
   - simple parameter type names in keys
5. Treat Java records and annotation-processor sources as indexer work items. Both produce silent gaps, and records are increasingly common in newer code.
6. Re-run this benchmark after the next round of changes, with a few new questions that cover records, MapStruct and field writes, so that the known limits are measured rather than assumed.
