# Sourcetrail CLI for coding agents — requirements

Oct 30, 2026 · Markus

## Purpose

An agent should get one exact answer to "who calls this", "what implements that" or "how does A reach B". Today that takes 5–10 rounds of grep and file reads, and the answers still miss overloads, overrides and generated code.

The Sourcetrail index already holds the answers: every symbol, every typed link between symbols and every place each one appears. The CLI's job is to make those queries fresh, cheap and short.

## Success criteria

Tokens saved is the main number, but it only counts on answers that are still correct. A cheap wrong answer costs more later than it saves now.

Measure it with a fixed benchmark: 10–15 real questions on one or two real repos (this fork and a Spring/Gradle project), each with a known correct answer. The agent answers every question twice, once with grep only and once with the CLI.

| Metric | Why it matters | Starting target |
| --- | --- | --- |
| Correct answers | Tokens saved on a wrong answer are worthless | No worse than grep; better on overload, override and Lombok questions |
| Tokens per question | Cost, and the space left in the context | At least 50% fewer (median) |
| Time per question, start to answer | Each tool call is a full model round trip, so fewer calls should make it faster, not just cheaper | At least 30% faster (median), including refresh |
| Tool calls per question | The main driver of both tokens and time | At least 50% fewer (median) |
| Startup time per command | Every step of a pipeline pays it | Under 200 ms |
| Query time, index already open | Deep `path` and `impact` queries are the risk | Under 1 s; `path --depth 6` under 2 s |
| Changed-files refresh | Runs before most first commands | Under 5 s for a few changed files |
| Used without prompting | A tool the agent never picks saves nothing | Used on at least 80% of the questions it suits |

The question set covers callers of an overloaded method, implementations of an interface, a controller-to-repository path, tests that reach a line, the impact of a signature change, and callers of a Lombok getter. Add two or three plain text-search questions as well, to check the agent still uses grep where grep is the right tool.

## Must-haves

Without these, an agent goes back to grep. Freshness comes first, because an index that's out of date gives confident wrong answers.

| Command | Answers | Why grep can't |
| --- | --- | --- |
| Auto-refresh (runs before every query) | Is the index current? If not, run the changed-files index first. | Grep has no index to go out of date. This is what the CLI has to match. |
| `find <name>` | Symbol by partial or simple name, with optional `--kind`. Returns kind, full name, signature and definition `file:line`. | Same-name symbols across packages, overloads |
| `refs <symbol>` | Every place it appears, grouped by the symbol that uses it ("in `OrderService.place()` at `OrderService.java:88`") | Comments, strings and look-alike names match too |
| `callers` / `callees <symbol> [--depth N]` | Who calls this, or what it calls, N levels deep. Takes overrides into account: calls through an interface method count as calls to its implementations, and are marked "via `Iface.m`". | Overloads, dynamic dispatch, Lombok getters |
| `impls` / `supers <type or method>` | Subtypes and overriding methods (down), supertypes and overridden methods (up) | Inheritance across files, generic types |
| `path <from> <to> [--depth N]` | Call chains from one symbol to another across layers, shortest first. Each hop shows `file:line`. Classes are expanded to their members. | No grep equivalent at all |

**Piping is the core design.** Each command reads symbols from stdin and writes symbols to stdout. One shell command can then answer a multi-step question, so the agent makes one tool call instead of three or four.

- **Output doubles as input.** The first column is the symbol's key, and the next command reads only that column. With no argument, every command reads stdin.
- **Sets in, sets out.** Each command takes a set of symbols and returns a set without duplicates. Empty input gives empty output and exit code 1, so the pipeline stops.
- **`filter` narrows a set between steps:** `--kind`, `--package`, `--annotated`, `--file`, `--not`.
- **`symbols-in` brings in input from elsewhere.** It turns file paths, such as `git diff --name-only` output, into the symbols defined in those files.
- **The index stays fixed during a pipeline.** Only the first command refreshes. Keys are full name + signature, not numeric ids, so they stay valid even after a refresh.
- **Counts per step on stderr** (`impls: 4 → callers: 37 → filter: 2`). When a pipeline returns nothing, this shows which step lost the results.
- **Ambiguous names list the candidates.** The CLI never picks one silently.

Examples:

```
# tests that reach a line
st at OrderService.java:120 | st callers --depth 5 | st filter --annotated Test

# endpoints that end up using a repository
st find --kind interface Repository | st impls | st callers --depth 3 | st filter --annotated RestController

# tests to run for a change
git diff --name-only | st symbols-in | st impact | st filter --annotated Test

# path between two layers, shortest first
st path OrderController OrderRepository --depth 6
1. OrderController.create(OrderDto)            OrderController.java:42
   -call->     OrderService.place(Order)        OrderService.java:88
   -call->     InventoryPort.reserve(Sku)       InventoryPort.java:12
   -override-> InventoryAdapter.reserve(Sku)    InventoryAdapter.java:30
   -call->     OrderRepository.save(Order)      InventoryAdapter.java:37
(+3 more paths, --limit)
```

## Should-haves

These make the must-haves sharper or save whole file reads.

| Feature | What it gives the agent |
| --- | --- |
| `--edges` / `--kinds` filters on every graph query | Choose link types (`call,usage,override,type-usage,inheritance,import,annotation`) and symbol kinds (`method,field,class,…`). Example: "path over calls only". |
| `at <file>:<line>` | The symbol at a location. Turns a stack-trace line into a symbol, then into its callers. |
| `outline <file or class>` | Members with kind, visibility, signature and line range. Replaces reading a 600-line file to find one method. |
| `impact <symbol> [--depth N]` | Everything that can reach this symbol (callers of callers, type users), grouped by file and package, with counts. Answers "what breaks if I change this?" |
| `deps <class or package> [--in or --out]` | Dependencies rolled up to class or package level. Spots layering violations (e.g. `domain` → `web`). |
| `--context N` | N lines of source around each result (one line is shown by default) |
| `status` | When the index was built, how many files are out of date, error count, languages |
| `errors [file]` | Files that failed or only partly indexed, so the agent knows where answers may be incomplete |
| External symbols marked and hidden by default | Library symbols with no definition are marked `ext`. `--include-external` shows them. |
| `--json` | Same results as structured data, for scripts and other tools |
| Batch mode | Several queries in one process start (`st batch < queries.txt`), to save startup time |

## Nice-to-haves

| Feature | What it gives the agent |
| --- | --- |
| MCP server wrapper | The same commands as tools, with the database kept open between calls |
| `diff-impact [git ref]` | Symbols changed in a git diff plus their callers. For code review and choosing which tests to run. |
| `annotated <@Annotation>` | Everything carrying an annotation, e.g. `@Transactional`, `@RestController`, `@Scheduled` |
| Lombok mapping | Generated members point back to the field or annotation line, marked `(lombok)` |
| `hierarchy <type>` | The full type tree printed as an indented outline |
| `cycles [--level class or package]` | Dependency cycles |
| `unused [--kind method]` | Symbols nothing references, as candidate dead code. Leaves out overrides, entry points and framework-annotated code. |
| `--format mermaid or dot` | Path and dependency results as a graph a human can view |

## Output and behaviour conventions

Every line of output costs the agent tokens, so the default output is terse and the same shape every time.

- **One line per result, tab-separated:** `<full name + signature>  <kind>  <relative path>:<line>  <source line>`. The first column is the key the next command reads.
- **Source line on every result:** the trimmed line of code at that location, which often makes a file read unnecessary. `--no-source` turns it off.
- **Readable names:** shown as `pkg.Class.method(Type)`.
- **Results on stdout only:** progress, refresh notices and per-step counts go to stderr, one line each (e.g. `refreshed 3 files in 1.2s`).
- **Stable order:** by path, then by line, so two runs can be compared.
- **Default limits:** 50 results. `callers` and `callees` go 1 level deep, `path` 6 levels. Results past the cap end with `+N more (--limit)`.
- **Finds its own project:** looks for the `.srctrlprj` file in the current directory and upward, the way git finds `.git`. `--project` sets it explicitly.
- **Speed:** under 1 second per query. Refresh time is reported separately.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Results found |
| 1 | Query ran, no results |
| 2 | Ambiguous name. Candidates are listed. |
| 3 | Symbol not found |
| 4 | No index for this project |
| 5 | Index out of date and refresh turned off or failed |

## Expected behaviour

Answers should match what the code does at runtime, as closely as static analysis allows. These are the cases where a naive tool gets it wrong.

- **Calls through an interface or abstract method reach the implementations.** `callers`, `callees` and `path` follow them and mark the hop "via `Iface.m`".
- **Method references count as calls.** `stream().map(this::convert)` shows up in `callers convert`.
- **Calls inside lambdas belong to the method that contains the lambda.**
- **A class stands for all its methods.** Given a class, `path`, `callers` and `callees` search from every member and report class-level results.
- **Lombok-generated members behave like hand-written ones.** Callers of a generated getter or builder are found.
- **Injected dependencies are traced end to end.** A Spring-style call through an interface field with one implementation shows up in `path`.
- **Results stay correct after a refresh.** When a method's signature changes, callers in files that weren't edited are still found.
- **Known limit:** reads and writes of a field aren't told apart, so "who writes this field" isn't required.
