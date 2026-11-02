# `st` Agent Core (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `st`, a pipeable command-line tool that answers "who calls this / what implements that / how does A reach B" for Gradle Java projects. It refreshes the index automatically, prints one terse line per result keyed by full name + signature, and points at original (not delombok) source lines.

**Architecture:** `st` is a new executable next to `Sourcetrail` and `Sourcetrail_cli`. It links the existing `Sourcetrail_cli_queries` library and Sourcetrail's storage APIs (`PersistentStorage`, `getGraphForTrail`). Indexing still runs through the adjacent `Sourcetrail index` process. Gradle preparation results are cached so a refresh only reruns Gradle when build files change. Delombok output is written so that unchanged files keep their bytes and mtimes, so Sourcetrail's existing changed-file detection skips them. Every query loads the index in-process; the resident process is Phase 3.

**Tech Stack:** C++17, Qt5 Core (QCoreApplication, QLockFile, QProcess), Catch2 (`src/external/catch`), CMake, Bash + jq for the smoke test, Lombok delombok via the Java bridge.

**Spec:** `docs/superpowers/specs/2026-10-30-sourcetrail-cli-requirements.md`. Background and reuse map: `docs/superpowers/specs/2026-10-30-sourcetrail-cli-implementation-notes.md`.

**Phases (only Phase 1 is planned here):**
1. Agent core: this plan.
2. Architecture questions: `impact`, `deps`, `outline`, `at`, `symbols-in`, `filter`, `--edges`, `--context`, `--include-external`, `errors`.
3. Speed: resident process, `batch`.

## Global Constraints

- Results go to stdout only. Progress, refresh notices and per-step counts go to stderr, one line each (e.g. `refreshed 3 files in 1.2s`).
- One line per result, tab-separated: `<full name + signature>\t<kind>\t<relative path>:<line>\t<source line>`. The first column is the key the next command reads.
- Readable names: `pkg.Class.method(Type)`, with no return type.
- Source line on every result: the trimmed line of code. `--no-source` turns it off.
- Stable order: by path, then by line (except `path`, which is shortest chain first).
- Default limits: 50 results. `callers` and `callees` default to depth 1, `path` to depth 6. Results past the cap end with `+N more (--limit)`.
- Finds its own project: looks for the `.srctrlprj` file in the current directory and upward. `--project` sets it explicitly.
- Exit codes: 0 results found, 1 query ran with no results, 2 ambiguous name (candidates listed), 3 symbol not found, 4 no index for this project, 5 index out of date and refresh turned off or failed. Command-line usage errors exit 64.
- Sets in, sets out. Empty input gives empty output and exit code 1.
- Ambiguous names list the candidates; the CLI never picks one silently.
- Calls through an interface method reach the implementations and are marked "via `Iface.m`". Method references (stored as usage links) count as calls.
- A class given to `callers`, `callees` or `path` stands for all of its methods.
- External symbols (no definition in the index) are hidden.
- Do not change the JSON `Sourcetrail_cli` commands' behaviour; `st` is a separate binary.
- Match the surrounding code style: tabs, `m_` members, `utility::` helpers, Catch2 `TEST_CASE("lowercase description")`.

## Decisions made in this plan (the spec leaves them open)

- **Project registry.** Your `.srctrlprj` files live in `~/Projects/sourcetrail/...`, not inside the source repos, so walking upward from a repo would never find them. `st` keeps a registry at `~/.config/sourcetrail/st-projects.tsv` (`<source root>\t<project file>`). Any run with `--project` registers that project's Gradle root, and discovery checks the registry for every directory it walks through.
- **Refresh in pipelines.** A command that reads stdin reads it to EOF *before* checking freshness. By then the upstream step has finished its own refresh, so later steps find the index fresh and skip it. `git diff | st ...` still refreshes, because git doesn't refresh anything. No marker or environment variable is needed.
- **Freshness check.** The index is stale if any source file, source directory or Gradle build file has an mtime at or after the last completed index time (`StorageStats::timestamp`). Directory mtimes catch deleted and renamed files. Source roots come from the Gradle preparation cache (Task 2). For other source group types, `st` checks only the indexed files' mtimes and existence, and says so on stderr.
- **Lombok locations.** Locations in `.../delombok/<set>/<rel>` map to `<original root>/<rel>`. The line is mapped by aligning the two files' trimmed lines (LCS). A location whose line has no match is printed with the delombok path and marked `(generated)` instead of guessing a line.
- **Lock.** Queries wait up to 30 s for the project lock (`--wait` ms), instead of failing at once.
- **Ambiguity output.** Candidates go to stderr as normal result lines, stdout stays empty, exit 2.
- **"via" marker.** When a result was reached across an override link, the kind column reads `method via <key of the interface method>`.

## Review Focus

1. Keys of generic methods, nested and anonymous classes (`<E>`, `, `, `anonymous class (File.java<80:44>)`) must survive a stdin round trip unchanged. Test: Task 3 `st keys round-trip generic and anonymous names`.
2. Paths containing spaces or apostrophes (your `o'neil` project folder) and files outside the current directory print correctly. Test: Task 3 `st relative path keeps spaces and apostrophes`.
3. Stdin with blank lines, CRLF endings or an unknown key: blank lines and `\r` are ignored, unknown keys are reported on stderr and skipped, and an all-empty input exits 1. Test: Task 3 `st reads keys from stdin ignoring blanks and carriage returns`.
4. Index missing, or locked by a running GUI refresh: exit 4, or wait and then exit 5 with a clear stderr line; never print stale results without saying so. Test: Task 6 smoke steps "busy lock" and "no index".
5. A Lombok location whose original file was deleted or whose line doesn't exist in the original falls back to the delombok path with `(generated)`; it never prints a wrong original line. Test: Task 5 `lombok location without a matching original line stays generated`.

---

## Prerequisite (decision for Markus, before Task 1)

The JSON CLI (`src/cli/`, `testing/cli/`, the `Sourcetrail_cli` CMake block and its storage changes) is uncommitted in the working tree. `st` builds on it. Commit that work, or say how you want it committed, before execution starts. Execution then branches from that commit.

---

### Task 1: Delombok output that only changes when the code changes

**Files:**
- Modify: `src/lib_java/utility/utilityGradle.cpp` (`runDelombok`, around lines 153–209)
- Create: `src/lib/utility/file/utilityDirectorySync.h`, `src/lib/utility/file/utilityDirectorySync.cpp`
- Test: `src/test/DirectorySyncTestSuite.cpp` (add to the list in `src/test/CMakeLists.txt`)

**Interfaces:**
- Produces: `namespace utility { struct DirectorySyncResult { size_t written = 0; size_t unchanged = 0; size_t removed = 0; }; DirectorySyncResult syncDirectoryContents(const FilePath& sourceDirectory, const FilePath& targetDirectory); }`. Files whose bytes are equal stay untouched (mtime kept); changed or new files are copied; files missing from the source are removed from the target, along with directories left empty.

- [ ] **Step 1: Write the failing tests** in `DirectorySyncTestSuite.cpp`, using a temp folder under `data/DirectorySyncTestSuite/tmp`:
  - `directory sync keeps identical files untouched`: target `a/A.java` mtime set 10 s in the past, same bytes in the source → after the sync, the mtime is unchanged, `unchanged == 1`, `written == 0`.
  - `directory sync rewrites changed files and adds new ones`: changed bytes in `A.java` plus a new `b/B.java` → both match the source, `written == 2`.
  - `directory sync removes files missing from the source`: target-only `old/Old.java` is gone, `old/` is removed, `removed == 1`.
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/Release --target Sourcetrail_test -j 8 && (cd build/Release/test && ./Sourcetrail_test "directory sync*")`. Expected: compile error (`syncDirectoryContents` undeclared).
- [ ] **Step 3: Implement `syncDirectoryContents`.** Compare bytes with `TextAccess`/`std::ifstream`. Write through `FileSystem::copyFile` after removing the target file.
- [ ] **Step 4: Change `runDelombok`.** Write into a sibling temp directory `<output>.tmp` (cleared first) instead of clearing `<output>`, pass `-f generateDelombokComment:skip` before `-d`, then call `syncDirectoryContents(<output>.tmp, <output>)` and remove `<output>.tmp`. Log the counts with `LOG_INFO`.
- [ ] **Step 5: Run the tests to see them pass**, plus the full `Sourcetrail_test`. Expected: the new tests pass, and the failures are exactly the 94 already failing at baseline (save the junit list before Step 1 with `./Sourcetrail_test -r junit` and compare with `comm`).
- [ ] **Step 6: Check on a real project.** Using the scratch copy of shop (or a fresh copy; see `wisdom.md` on keeping `.git`), run `Sourcetrail_cli index` twice with one source edit in between. Expected: the second run's stderr reports `Finished indexing:` with only a handful of files, not ~560. Record the time.
- [ ] **Step 7: Commit** `feat(gradle): keep unchanged delombok output so refresh re-indexes only edited files`.

### Task 2: Cache Gradle preparation between refreshes

**Files:**
- Create: `src/lib_java/utility/utilityGradleCache.h`, `src/lib_java/utility/utilityGradleCache.cpp`
- Modify: `src/lib_java/utility/utilityGradle.cpp` (`gradleCopyDependencies`, `gradlePrepareLombokSources`, `gradleGetAllSourceDirectories` consult the cache), `src/lib_java/utility/utilityGradle.h` (expose `getDelombokOutputDirectory`)
- Test: `src/test/GradleCacheTestSuite.cpp` (add to `src/test/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  namespace utility {
  struct GradleProjectInfo {
      std::string fingerprint;                         // build-file fingerprint it was computed for
      bool includesTests = false;
      bool mainHasLombok = false, testHasLombok = false;
      FilePath mainLombokJar, testLombokJar;
      std::map<std::wstring, std::vector<FilePath>> sourceDirectories; // "main" / "test" -> original roots
  };
  std::string gradleBuildFingerprint(const FilePath& projectDirectory);
  std::optional<GradleProjectInfo> loadGradleProjectInfo(const FilePath& outputDirectory, const FilePath& projectDirectory, bool includesTests);
  void saveGradleProjectInfo(const FilePath& outputDirectory, const GradleProjectInfo& info);
  std::vector<FilePath> gradleBuildFiles(const FilePath& projectDirectory);
  }
  ```
  `gradleBuildFiles` lists `build.gradle`, `build.gradle.kts`, `settings.gradle`, `settings.gradle.kts`, `gradle.properties`, `gradle/libs.versions.toml` and `gradle/wrapper/gradle-wrapper.properties` at the root and in every subdirectory, skipping `build`, `.gradle`, `.git` and `node_modules`. The fingerprint is the sorted `path|size|mtime` lines of those files. The cache file is `<outputDirectory>/sourcetrail-gradle-project.cache`, in any line-based format you choose. `load` returns `nullopt` when the file is missing, or when the fingerprint, `includesTests` or any recorded source root no longer matches.
- Consumed by Task 4 (source roots for freshness) and Task 5 (source roots for Lombok mapping).

- [ ] **Step 1: Write the failing tests** (temp project folder under `data/GradleCacheTestSuite/tmp`, no Gradle needed):
  - `gradle cache round-trips project info`: save then load returns equal fields.
  - `gradle cache is invalidated by a build file change`: touch `sub/build.gradle` with a new mtime → `load` returns `nullopt`.
  - `gradle cache ignores files under build directories`: changing `build/build.gradle` keeps the cache valid.
  - `gradle cache is invalidated when test inclusion changes`: saved with `includesTests=false`, loaded with `true` → `nullopt`.
- [ ] **Step 2: Run them to see them fail** (compile error: unknown symbols).
- [ ] **Step 3: Implement the four functions.**
- [ ] **Step 4: Use the cache in `utilityGradle.cpp`.**
  - When `loadGradleProjectInfo` succeeds **and** the output folder already holds jars, skip `copyCompileLibs`/`copyTestCompileLibs`, `getLombokInfo` and `getSourceDirectoriesBySourceSet`, and use the cached values.
  - Delombok still runs (Task 1 keeps it cheap for the indexer).
  - After a real Gradle run, `saveGradleProjectInfo` records the fresh values.
  - `gradleGetAllSourceDirectories` must also come from the cache, so a refresh makes **zero** Gradle tooling calls when build files are unchanged.
- [ ] **Step 5: Run the new tests to see them pass**, then the full suite against the baseline list.
- [ ] **Step 6: Check on a real project.** Run the shop refresh twice with a source-only edit. Expected: the second run's log has no `InfoRetriever` / `gradleGetAllSourceDirectories` Gradle calls, and the total refresh is under 15 s (measured before: 56 s). Then edit `build.gradle`'s mtime and confirm Gradle runs again.
- [ ] **Step 7: Commit** `feat(gradle): reuse Gradle preparation when build files are unchanged`.

### Task 3: `st` executable: keys, output, input, discovery, exit codes, `find`, `status`

**Files:**
- Create: `src/cli/st/StKey.h/.cpp` (keys and name resolution), `src/cli/st/StOutput.h/.cpp` (result lines, sorting, limits, relative paths), `src/cli/st/StInput.h/.cpp` (stdin keys), `src/cli/st/StProject.h/.cpp` (discovery and registry), `src/cli/st/StMain.cpp` (argument parsing and dispatch)
- Modify: `CMakeLists.txt` (CLI block, around lines 760–783): add `add_library(Sourcetrail_st_lib STATIC ...)` linking `Sourcetrail_cli_queries`, `add_executable(st src/cli/st/StMain.cpp)` with `RUNTIME_OUTPUT_DIRECTORY` the same as `Sourcetrail_cli` (`app/`), `add_dependencies(st ${APP_PROJECT_NAME})`, a test executable `Sourcetrail_st_test`, and `add_test(NAME cli.st COMMAND Sourcetrail_st_test)`
- Modify: `src/cli/main.cpp`: move `reindex()` into `src/cli/Query.h/.cpp` as `void cli::reindex(const QString& project, bool full, int timeoutMs)` so both binaries share it
- Test: `src/cli/st/StTest.cpp`

**Interfaces:**
- Produces (namespace `st`):
  ```cpp
  std::wstring symbolKey(const NameHierarchy& name);        // qualified name + last element's parameter string, no return type
  enum class Resolution { Found, Ambiguous, NotFound };
  struct Resolved { Resolution status; std::vector<Id> ids; };
  Resolved resolveSymbol(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);
  // exact key -> exact qualified name (all overloads count as ambiguous unless text has "(") -> suffix match on qualified name ("Class.method", "method")
  struct ResultLine { std::wstring key; std::wstring kind; FilePath file; size_t line; std::string source; };
  struct OutputOptions { size_t limit = 50; bool showSource = true; FilePath cwd; };
  int printResults(std::vector<ResultLine> lines, const OutputOptions& options, std::ostream& out); // sort, dedupe, limit, returns exit code 0 or 1
  std::wstring displayPath(const FilePath& file, const FilePath& cwd); // relative when under cwd, else absolute
  std::vector<std::wstring> readKeys(std::istream& in);    // first column of each non-blank line, "\r" stripped, duplicates removed in order
  FilePath findProject(const FilePath& cwd, const FilePath& registryFile);  // empty FilePath if none
  void registerProject(const FilePath& registryFile, const FilePath& project); // reads Gradle root from ProjectSettings
  ```
  Exit codes as in Global Constraints, as `enum ExitCode { EXIT_RESULTS = 0, EXIT_EMPTY = 1, EXIT_AMBIGUOUS = 2, EXIT_NOT_FOUND = 3, EXIT_NO_INDEX = 4, EXIT_STALE = 5, EXIT_USAGE = 64 };` in `StOutput.h`.
- Command line: `st <command> [symbol|name] [--project P] [--limit N] [--depth N] [--kind K] [--no-source] [--no-refresh] [--wait MS]`. When the positional argument is missing and stdin is not a TTY, symbols come from `readKeys(std::cin)`. In this task `find <name> [--kind K]` and `status` work; the other commands are added in Task 4.
- `find` lists every symbol `getAutocompletionSymbolMatches` returns for the name (filtered by `--kind`, external symbols dropped), one result line each, at its definition location (scope location preferred, else first token location).
- `status` prints `indexed_at`, `files`, `errors`, `stale_files` (from Task 4; print `unknown` until then) as `key\tvalue` lines.

- [ ] **Step 1: Write the failing tests** in `StTest.cpp`, using in-memory storage built like `src/cli/QueryTest.cpp` (`addMethod` helper with `NAME_DELIMITER_JAVA`, signature via `NameElement(name, prefix, postfix)`):
  - `st key omits return type and keeps parameters`: method `java.lang.String example.Person.getName()` → `example.Person.getName()`; class `example.Person` → `example.Person`.
  - `st keys round-trip generic and anonymous names`: names `verifyAndCaptureSave<E>` with parameter `(java.util.List<E>, int)` and `dummy.anonymous class (A.java<80:44>).consumeAll(java.util.Collection<E>)`: `resolveSymbol(storage, symbolKey(h), all)` returns exactly the original id.
  - `st resolves overloads as ambiguous and exact keys as found`: `run(int)` and `run(long)` → `resolveSymbol("example.Job.run")` is `Ambiguous` with 2 ids, `resolveSymbol("example.Job.run(int)")` is `Found`, `resolveSymbol("Job.run(long)")` is `Found`, `resolveSymbol("nope")` is `NotFound`.
  - `st reads keys from stdin ignoring blanks and carriage returns`: input `"a.B.c()\tmethod\tx:1\tsrc\r\n\n  \na.B.c()\r\nd.E\n"` → `{L"a.B.c()", L"d.E"}`.
  - `st relative path keeps spaces and apostrophes`: cwd `/home/u/o'neil/repo`, file `/home/u/o'neil/repo/src/My File.java` → `src/My File.java`; file outside cwd → absolute.
  - `st output sorts by path and line and reports overflow`: 3 lines with limit 2 → two sorted lines, then `+1 more (--limit)`, returns 0; zero lines → returns 1 and prints nothing.
  - `st finds project upward and through the registry`: temp dirs `repo/sub` with `repo/x.srctrlprj` → found from `repo/sub`; a separate `src/repo2/sub` with registry line `src/repo2\t/p/y.srctrlprj` (where `/p/y.srctrlprj` exists) → found.
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/Release --target Sourcetrail_st_test -j 8 && ctest --test-dir build/Release -R cli.st --output-on-failure`. Expected: compile errors for the missing headers.
- [ ] **Step 3: Implement `StKey`, `StOutput`, `StInput` and `StProject`** with the signatures above. Resolution uses `getAutocompletionSymbolMatches(text-before-'(', NodeTypeSet::all(), 1000, 0)` and then filters on exact key or qualified name / suffix.
- [ ] **Step 4: Implement `StMain.cpp`:**
  - argument parsing and project discovery (`--project` → `registerProject`; otherwise `findProject(cwd, ~/.config/sourcetrail/st-projects.tsv)`, exit 64 with a hint if none);
  - stdin read before any freshness check;
  - `QLockFile` wait with `--wait` (default 30000);
  - missing or temporary index → exit 4;
  - read-only `PersistentStorage` + `buildCaches()`;
  - `find` and `status`;
  - per-command count on stderr, `find: 3`.
  
  Move `reindex` into `Query.cpp` and make `src/cli/main.cpp` call `cli::reindex`.
- [ ] **Step 5: Run the tests to see them pass.** Then confirm `ctest -R cli.queries` and `bash testing/cli/smoke.sh build/Release/app/Sourcetrail_cli` (JAVA_HOME=`~/.jdks/temurin-21.0.3`) still pass.
- [ ] **Step 6: Commit** `feat(st): add st binary with keys, discovery, find and status`.

### Task 4: Auto-refresh and the graph commands (`refs`, `callers`, `callees`, `impls`, `supers`, `path`)

**Files:**
- Create: `src/cli/st/StFreshness.h/.cpp`, `src/cli/st/StGraph.h/.cpp`
- Modify: `src/cli/st/StMain.cpp`, `src/cli/st/StTest.cpp`

**Interfaces:**
- Consumes: Task 2 `loadGradleProjectInfo`, `gradleBuildFiles`; Task 3 `resolveSymbol`, `symbolKey`, `ResultLine`, `printResults`, `cli::reindex`.
- Produces:
  ```cpp
  struct Freshness { bool stale = false; size_t changedFiles = 0; std::string reason; };
  Freshness checkFreshness(const ProjectSettings& settings, const TimeStamp& indexedAt, const std::vector<FilePath>& indexedFiles);
  enum class Direction { Incoming, Outgoing };
  std::vector<ResultLine> references(PersistentStorage&, const std::vector<Id>& symbols);              // refs
  std::vector<ResultLine> calls(PersistentStorage&, const std::vector<Id>& symbols, Direction, size_t depth); // callers / callees
  std::vector<ResultLine> hierarchy(PersistentStorage&, const std::vector<Id>& symbols, Direction);      // impls (Incoming) / supers (Outgoing)
  struct PathHop { Id from; Id to; Edge::EdgeType type; FilePath file; size_t line; };
  std::vector<std::vector<PathHop>> paths(PersistentStorage&, const std::vector<Id>& from, const std::vector<Id>& to, size_t depth, size_t limit);
  std::vector<Id> expandClasses(PersistentStorage&, const std::vector<Id>& symbols); // classes/interfaces/enums -> their methods (MEMBER children); others unchanged
  ```
- Rules the implementer cannot infer:
  - Refresh: after stdin is read and before opening storage, run `checkFreshness`. If stale and `--no-refresh` isn't set, call `cli::reindex(project, false, 600000)` and print `refreshed N files in X.Ys` on stderr. If stale and `--no-refresh` is set, or reindex fails, exit 5.
  - `calls`: link set `EDGE_CALL | EDGE_USAGE | EDGE_OVERRIDE`. Traverse once per expanded symbol and merge: callers use `getGraphForTrail(0, id, ~NodeKindMask(0), edges, false, depth, true)`, callees use `getGraphForTrail(id, 0, ~NodeKindMask(0), edges, false, depth, true)`. `nodeNonIndexed=false` drops external symbols. Output only method, function and constructor nodes, excluding the input symbols.
    - Each result's location is the call site: the location of the link that reached it (for callers, the location in the caller).
    - If the hop that reached a node was an override link, the kind column is `method via <symbolKey of the overridden method>`.
  - `references`: incoming links of every kind except `EDGE_MEMBER`, from storage edges directly (not `getGraphForActiveTokenIds`, which merges class links; see the earlier finding). One result line per reference location, keyed by the *using* symbol.
  - `hierarchy`: for types `EDGE_INHERITANCE`, for methods `EDGE_OVERRIDE`, depth 0 (unlimited; safe after commit `044525e8`).
  - `paths`: links `EDGE_CALL | EDGE_USAGE | EDGE_OVERRIDE`, one `getGraphForTrail(from, to, …, depth)` per (from × to) pair after `expandClasses` (cap the pairs at 200 and say so on stderr). From each trail graph, enumerate **simple** paths from origin to target, breadth-first and shortest first, stopping at `limit`. Nodes not on any simple path never appear. Print the spec's chain format:
    ```
    1. <from key>\t<file>:<line>
       -call-> <key>\t<file>:<line>
    ```
    with `(+N more paths, --limit)` when capped.
- [ ] **Step 1: Write the failing tests** (in-memory storage with file nodes and token locations like `addJavaFile`/`addTokenLocation` in `src/test/StorageTestSuite.cpp`):
  - `st callers climb from implementation through interface`: `Impl.run()` overrides `Iface.run()`; `Client.go()` calls `Iface.run()` → `calls({Impl.run}, Incoming, 1)` contains `Client.go()` with kind `method via Iface.run()` and the call site's line.
  - `st callers count method references`: `Stream.use()` has a USAGE link to `Conv.convert(int)` → it appears in callers of `convert`.
  - `st callers of a class cover its methods`: class `Svc` with members `a()`, `b()`; `X.x()` calls `b()` → callers of `Svc` contain `X.x()`.
  - `st callees hide fields and external symbols`: `A.f()` uses field `A.count` and calls external `java.util.List.size()` and internal `B.g()` → callees are exactly `B.g()`.
  - `st refs list each reference location by using symbol`: two calls from `X.x()` on lines 4 and 9 → two result lines.
  - `st impls and supers follow inheritance transitively`: `C extends B extends A` → impls of `A` = {B, C}; supers of `C` = {B, A}.
  - `st path returns shortest simple chains first and drops off-path nodes`: `A.a→B.b→D.d` and `A.a→C.c→E.e→D.d`, plus `Z.z` overriding `A.a` → two chains, the 3-node one first, and `Z.z` absent.
  - `st freshness detects edits, new files and deletions`: temp source root plus an `indexedAt` timestamp in the past → stale after touching a file, after adding a file, after deleting a file (directory mtime); fresh when nothing changed.
- [ ] **Step 2: Run them to see them fail.** `ctest --test-dir build/Release -R cli.st --output-on-failure`. Expected: compile errors for the `StGraph`/`StFreshness` symbols.
- [ ] **Step 3: Implement `StFreshness`.** Source roots and build files come from `loadGradleProjectInfo`, located through each enabled `SourceGroupSettingsJavaGradle` (`getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory()`, `getGradleDependenciesDirectoryPath()`). Fallback: the indexed files.
- [ ] **Step 4: Implement `StGraph`**, then wire `refs`, `callers`, `callees`, `impls`, `supers`, `path` and auto-refresh into `StMain.cpp`.
- [ ] **Step 5: Run the tests to see them pass**, then the `cli.queries` ctest.
- [ ] **Step 6: Commit** `feat(st): add auto-refresh and graph commands`.

### Task 5: Map Lombok locations back to the original source

**Files:**
- Create: `src/cli/st/StLombok.h/.cpp`
- Modify: `src/cli/st/StOutput.cpp` (map each `ResultLine` before printing), `src/cli/st/StTest.cpp`

**Interfaces:**
- Consumes: Task 2 `GradleProjectInfo::sourceDirectories`, `getDelombokOutputDirectory`.
- Produces:
  ```cpp
  std::vector<std::optional<size_t>> alignLines(const std::vector<std::string>& generated, const std::vector<std::string>& original);
  // generated line index -> original line index; LCS over whitespace-trimmed, non-empty lines; unmatched -> nullopt
  struct LombokRoots { std::vector<std::pair<FilePath, std::vector<FilePath>>> delombokToOriginalRoots; };
  LombokRoots lombokRoots(const ProjectSettings& settings);
  ResultLine toOriginal(const ResultLine& line, const LombokRoots& roots, const std::function<std::vector<std::string>(const FilePath&)>& readLines);
  ```
  `toOriginal` replaces `file` and `line` when the generated line has a match. Otherwise it keeps the delombok path and appends ` (generated)` to the kind. Per-file alignments are cached for the duration of one command.
- [ ] **Step 1: Write the failing tests:**
  - `lombok alignment maps reformatted code to original lines`: generated `{"// header","package p;","","public class A {","  private final String n;","  @lombok.Generated","  public String getN() {","    return this.n;","  }","  void run() {","    go();","  }","}"}` vs original `{"package p;","import lombok.Getter;","@Getter","public class A {","    private final String n;","    void run() {","        go();","    }","}"}` → generated 10 (`void run() {`) → original 5, generated 11 (`go();`) → 6.
  - `lombok location without a matching original line stays generated`: generated line 7 (`public String getN() {`) → `nullopt`, and `toOriginal` keeps the delombok path with kind suffix ` (generated)`.
  - `lombok location of a deleted original stays generated`: no original file found under any root → unchanged path and ` (generated)`.
- [ ] **Step 2: Run them to see them fail** (compile error).
- [ ] **Step 3: Implement** `alignLines` (O(n·m) DP is fine for source files; cap at 5000 × 5000 lines, otherwise return all `nullopt`), `lombokRoots` and `toOriginal`.
- [ ] **Step 4: Run the tests to see them pass**, then all `cli.st` tests.
- [ ] **Step 5: Commit** `feat(st): report Lombok results at original source lines`.

### Task 6: End-to-end smoke test, docs, and real-project check

**Files:**
- Modify: `testing/cli/lombok/src/main/java/example/Caller.java` (implement the new interface)
- Create: `testing/cli/lombok/src/main/java/example/Greeter.java` (interface `String greeting(Person person);`), `testing/cli/lombok/src/main/java/example/Welcome.java` (field `Greeter greeter`, method `String welcome(Person p) { return greeter.greeting(p); }`)
- Create: `testing/cli/st-smoke.sh`
- Modify: `README.md` (an `st` section next to the `Sourcetrail_cli` section), `wisdom.md` (any lesson learned)

- [ ] **Step 1: Write `testing/cli/st-smoke.sh`.** Same isolation as `testing/cli/smoke.sh` (temp HOME, copied fixture, JDK 21 `JAVA_HOME`, `jq` not needed). It runs `st` from inside the copied fixture with no `--project` after one registering run, and asserts:
  - `st find getName` exits 0 and its first column is `example.Person.getName()`;
  - `st callers example.Caller.greeting(example.Person)` lists `example.Welcome.welcome(example.Person)` with kind `method via example.Greeter.greeting(example.Person)`;
  - `st find getName | st callers` lists `example.Caller.greeting(example.Person)`, and the printed path is `src/main/java/example/Caller.java` with the same line `grep -n 'getName()' src/main/java/example/Caller.java` reports;
  - `st path example.Welcome example.Person.getName()` prints a chain starting `1. example.Welcome.welcome(example.Person)`;
  - after editing `Caller.java` (add a method calling `getName`), the next `st callers example.Person.getName()` prints `refreshed` on stderr and lists the new method; an immediately repeated command prints no `refreshed`;
  - exit codes: `st callers nope` → 3, `st callers greeting` with two candidates (add an overload `greeting(Person, String)` in the fixture) → 2, `printf '' | st callers` → 1, a busy lock file (as in `smoke.sh`) with `--wait 500` → 5, a project without an index → 4.
- [ ] **Step 2: Run it.** `JAVA_HOME=$HOME/.jdks/temurin-21.0.3 bash testing/cli/st-smoke.sh build/Release/app/st`. Expected: final line `st smoke passed`. Fix whatever fails in the owning task's code, not in the test.
- [ ] **Step 3: Check on a real project (manual, results go in the PR description).** On the scratch copies of shop and batch, with the measured numbers:
  - first `st` command latency;
  - refresh after a one-file edit (target: < 15 s shop, < 6 s batch);
  - `st callers ...OrderReceiverService.execute(java.util.UUID)` returns the 6 production callers at original-file line numbers;
  - `st path ...CreateOrderV3CommandService.create(...) ...CustomerChangeEventProcessor.consume(...)` prints the interface/override/method-reference chain.
- [ ] **Step 4: Update `README.md`.** Install (symlink `build/Release/app/st` onto PATH), discovery and registry, the output format, the piping examples from the spec, exit codes, and the Lombok mapping and its `(generated)` fallback. Keep the existing Lombok line-number note for `Sourcetrail_cli`, which still reports delombok paths.
- [ ] **Step 5: Run everything.**
  - `ctest --test-dir build/Release --output-on-failure`
  - the full `Sourcetrail_test` against the baseline list
  - `bash testing/cli/smoke.sh build/Release/app/Sourcetrail_cli`
  - `bash testing/cli/st-smoke.sh build/Release/app/st`
  
  Report any failure by name.
- [ ] **Step 6: Commit** `test(st): end-to-end smoke test and README`.
