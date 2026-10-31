# `st` Architecture Questions (Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `st` the commands an agent needs for change and design questions: `filter`, `symbols-in`, `at`, `outline`, `impact` and `deps`, plus `--edges` on `path` and `impact`. Also close two Phase 1 gaps: `buildSrc` edits go unnoticed, and a symlinked checkout breaks project discovery.

**Architecture:** Every new command lives in a focused module under `src/cli/st/` (`StFilter`, `StFiles`, `StArchitecture`), linked into `Sourcetrail_st_lib`. `StMain.cpp` only parses arguments and dispatches. Graph walks read edges level by level through two new `PersistentStorage` accessors, not through `getGraphForTrail`. Lombok projects take original paths and lines as input and map them to the delombok copy that is actually indexed. This is the reverse of Phase 1's output mapping, under the same "never a guess" rule.

**Tech Stack:** C++17, Qt5 Core, Catch2, CMake, Bash + the `testing/cli/lombok` Gradle fixture.

**Spec:** `docs/superpowers/specs/2026-10-30-sourcetrail-cli-requirements.md`. Phase 1 plan with its decisions: `docs/superpowers/plans/2026-10-30-st-agent-core.md`.

**Not in this plan:** `--context`, `--include-external`, `errors`, `--json` (later), resident process, `batch` and the opt-in `st --watch` (Phase 3).

## Global Constraints

Everything in the Phase 1 plan's Global Constraints still holds. In particular:
- Output format is one tab-separated line per result: `<key>\t<kind>\t<relpath>:<line>\t<source>`.
- Results go to stdout; one-line progress and counts (`impact: 37`) go to stderr.
- Results print in path then line order.
- The default limit is 50, followed by `+N more (--limit)`.
- Exit codes are 0/1/2/3/4/5, plus 64 for usage errors and 70 for internal errors.
- External symbols stay hidden.
- Code style is tabs, `m_` members and `TEST_CASE("lowercase description")`.

New:
- **Command inputs:**
  - `filter`, `impact` and `deps` take symbols, either as arguments or as keys on stdin.
  - `symbols-in` takes file paths.
  - `at` takes `<file>:<line>`. Anything after a second colon is ignored, so `grep -n` output pipes straight in.
  - `outline` takes either kind of input. An item naming an existing file is treated as a file; anything else is a symbol.
- **Options limited to one command are a usage error (64) on any other command:** `--package`, `--annotated`, `--file`, `--not` (filter), `--in` (deps), `--group` (impact).
- **`--edges <list>`** is comma-separated. Names: `call`, `usage`, `type-usage`, `override`, `inheritance`, `import`, `annotation`. Any other name exits 64 and names the bad entry. `--edges` is accepted by `path` and `impact` only.
- **Lombok input rule:**
  - An original file path maps to its delombok copy.
  - An original line maps through `alignLines` in the reverse direction.
  - A line with no counterpart exits 3 with `st: <file>:<line> has no counterpart in the indexed (delombok) file`. Never fall back to a neighbouring line.
- **`ResultLine` gains `size_t endLine = 0`.** When `endLine > line`, the location column prints as `<path>:<line>-<endLine>`. `toOriginal` maps `endLine` as well and sets it to 0 when it can't be mapped.

## Decisions made in this plan (the spec leaves them open)

- **`filter`:**
  - Prints the input lines that pass unchanged, in input order. This keeps call sites and "via" marks.
  - Identical lines are printed once, and `--limit` applies.
  - All given criteria must hold. `--not` negates the combined condition.
  - `--annotated A` matches a symbol that carries `@A` itself, or whose enclosing type does. The annotation matches when its key equals `A` or ends with `.A`. This is what makes `filter --annotated RestController` keep controller methods.
  - `--package P` keeps keys that start with `P.`.
  - `--file G` is a Unix wildcard. A pattern containing `/` is matched against the displayed path of the symbol's definition; any other pattern is matched against the file name alone.
- **`symbols-in` and file `outline`:** they list every explicitly defined symbol in the file, except packages, files and type parameters, in line order. `outline` adds the line range (scope location) and visibility: the kind column reads `public method`, or just `method` when no access is stored.
- **`outline <class>`:** lists the class's direct members (member links) in line order. Nested types are listed, but their members aren't.
- **`at`:**
  - The result is the innermost defined symbol whose scope contains the line. If no scope contains it, the result is a symbol whose definition token is on the line, which covers fields.
  - A file argument that doesn't exist relative to the current folder is matched as a path suffix (`/OrderService.java`) against the indexed files, because stack traces give bare names.
  - If two files match, `at` exits 2 and lists them on stderr.
- **`impact`:**
  - Walks reverse dependencies:
    - users of the symbol (`call`, `usage`, `type-usage`, `annotation`);
    - subtypes (`inheritance`, walked from super to sub);
    - overrides in both directions (callers through an interface method are affected, and so are implementations of a changed interface method).
  - Override hops are free and every other hop costs 1. `--depth` defaults to 0, meaning unlimited.
  - A class input stands for the class and all of its members, recursively.
  - Each result line shows the site of the link that first reached it.
  - `--group file|package` prints one line per file or package instead. The key is the path or package name, the kind is `file, N symbols` or `package, N symbols`, and the location is the first symbol's location.
  - stderr always gets `impact: <symbols> symbols in <files> files, <packages> packages`.
- **`deps`:**
  - Rolls up to the level of its input. A package input gives one row per package; a type or method input gives one row per top-level type.
  - Default is outgoing; `--in` gives incoming.
  - Links inside the input (for example, a member calling a sibling) don't count.
  - External targets are hidden.
  - Counted link kinds: everything except member, import and include.
  - Row format: key = the target type or package, kind = `class, N links` or `package, N links`, location and source = the first link's site in path/line order.
- **`--edges`:** `path` defaults to `call,usage,override`, which is Phase 1's behaviour. `impact` defaults to `call,usage,type-usage,override,inheritance,annotation`.
- **Build fingerprint:**
  - These files count as Gradle build files: every regular file under `<root>/buildSrc` (skipping `build`, `.gradle` and `.git`), plus `$GRADLE_USER_HOME/gradle.properties` (or `~/.gradle/gradle.properties`) when it exists.
  - `gradleBuildFiles` returns them, so both the preparation cache and `st`'s freshness check see them.
- **Symlinks:**
  - `findProject` walks the current folder as given and also its canonical form.
  - Registry roots are compared in canonical form.
  - `displayPath` makes a path relative to `cwd`, and falls back to the canonical `cwd` before giving up and printing an absolute path.
- **Fixture:** `testing/cli/lombok` gains a second package, `example.web`. It contains `Endpoint.java`, an annotation, and `GreetingController.java`, annotated `@Endpoint`, with a method that calls `example.Caller`. The JSON smoke test's `completed_files` goes from 5 to 7.

## Review Focus

1. **Duplicate file names:** a bare file name from a stack trace that matches two indexed files (same name, two packages) must exit 2 and list both, never pick one. Test: Task 4, `st at lists both files when a bare name is ambiguous`.
2. **Lombok input paths and lines:** a Lombok file given by its original path resolves to the delombok copy, and results print at original lines. An original line with no generated counterpart exits 3 and is never moved to a neighbour. Tests: Task 4, `st at maps an original lombok line to the generated file` and `st at refuses an original line without a generated counterpart`.
3. **Cycles:** `impact` on a cyclic graph (A uses B uses A, mutual recursion) terminates and never reports its own input. Test: Task 6, `st impact terminates on cycles and leaves out its input`.
4. **Multi-line input to `filter`:** `refs` output (several lines per key) and `callers` output (with "via" marks) pass through `filter` line for line. The upstream `+N more` marker still triggers the truncation notice. Test: Task 3, `st filter keeps every passing input line unchanged`.
5. **Symlinked checkout:** running from a symlinked checkout finds the project through the registry and prints relative paths. Test: Task 2, `st finds the project and relative paths from a symlinked checkout`.

---

### Task 1: `buildSrc` and user Gradle properties count as build files

**Files:**
- Modify: `src/lib_java/utility/utilityGradleCache.cpp` (`isBuildFile`, `gradleBuildFiles`)
- Test: `src/test/GradleCacheTestSuite.cpp`

**Interfaces:**
- Produces: the signature of `std::vector<FilePath> gradleBuildFiles(const FilePath& projectDirectory)` stays the same. Its result now also contains `buildSrc` files and the user's `gradle.properties`.

- [ ] **Step 1: Write the failing tests** in `GradleCacheTestSuite.cpp`:
  - `gradle build files include buildSrc sources`: create `buildSrc/src/main/groovy/conventions.gradle` and `buildSrc/build/tmp/x.class` → the first is listed and the second isn't.
  - `gradle build fingerprint changes when the user gradle properties change`: point `GRADLE_USER_HOME` (`qputenv`) at a temp dir holding `gradle.properties`, rewrite it with a different size → the fingerprint differs.
  - `gradle build files skip a missing user gradle properties`: point `GRADLE_USER_HOME` at an empty dir → no entry outside the project directory.
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/Release --target Sourcetrail_test -j 8 && (cd build/Release/test && ./Sourcetrail_test "gradle build*")`. Expected: the new assertions fail.
- [ ] **Step 3: Implement.** Files under a `buildSrc` path component count as build files, unless they sit inside a skipped directory. Append the user properties file last, after sorting, when it exists. Restore `GRADLE_USER_HOME` at the end of each test.
- [ ] **Step 4: Run the tests to see them pass, then the full `Sourcetrail_test`.** Expected: the new tests pass. The failures are exactly the baseline set (save the junit list before Step 1 and compare with `comm`).
- [ ] **Step 5: Commit** `fix(gradle): count buildSrc and user gradle.properties as build files`.

### Task 2: Project discovery and paths work from a symlinked checkout

**Files:**
- Modify: `src/cli/st/StProject.cpp` (`findProject`), `src/cli/st/StOutput.cpp` (`displayPath`)
- Test: `src/cli/st/StTest.cpp`

**Interfaces:**
- Produces: the signatures stay the same. `displayPath(file, cwd)` now also tries the canonical `cwd`.

- [ ] **Step 1: Write the failing test** `st finds the project and relative paths from a symlinked checkout`:
  - Setup: `QTemporaryDir` with `real/src/A.java`, a symlink `link -> real`, and a registry line `<tmp>/real\t<tmp>/p.srctrlprj` (with `p.srctrlprj` existing).
  - `findProject(<tmp>/link/src, registry) == <tmp>/p.srctrlprj`.
  - `displayPath(<tmp>/real/src/A.java, <tmp>/link) == L"src/A.java"`.
  - A second registry entry written with the symlink path is found from the real path.
- [ ] **Step 2: Run it to see it fail.** `cmake --build build/Release --target Sourcetrail_st_test -j 8 && build/Release/test/Sourcetrail_st_test "*symlinked*"`.
- [ ] **Step 3: Implement** with `QFileInfo::canonicalFilePath`. Fall back to the given path when it doesn't exist.
- [ ] **Step 4: Run all `Sourcetrail_st_test`.** Expected: everything passes.
- [ ] **Step 5: Commit** `fix(st): find projects and print relative paths from symlinked checkouts`.

### Task 3: `filter`

**Files:**
- Create: `src/cli/st/StFilter.h`, `src/cli/st/StFilter.cpp`, `src/cli/st/StTestSupport.h`, `src/cli/st/StQueryTest.cpp`
- Modify: `src/cli/st/StInput.{h,cpp}`, `src/cli/st/StMain.cpp`, `src/cli/st/StTest.cpp`, `CMakeLists.txt` (add `StFilter.cpp` to `Sourcetrail_st_lib` and `StQueryTest.cpp` to `Sourcetrail_st_test`)

**Interfaces:**
- Produces:
  - In `StTestSupport.h`: the test helpers `Code`, `keys`, `lineFor` and `write` move here from `StTest.cpp`, as inline definitions in namespace `st::test` (named, so both test files share them). `Code` gains:
    - `void scope(Id symbol, const std::wstring& type, size_t first, size_t last)`, which adds a `LOCATION_SCOPE` start/end pair in that type's file;
    - `void access(Id symbol, AccessKind kind)`;
    - `Id annotate(Id symbol, Id annotation)`, which adds an `EDGE_ANNOTATION_USAGE` from the symbol to the annotation.
  - `struct InputLine { std::wstring key; std::string raw; }; std::vector<InputLine> readLines(std::istream& in, size_t* truncated = nullptr);`: non-blank lines with `\r` stripped, identical raw lines once, in order. `readKeys` is rebuilt on top of it with unchanged behaviour.
  - `struct FilterOptions { NodeKindMask kinds = ~NodeKindMask(0); std::wstring package; std::wstring annotation; std::wstring filePattern; bool invert = false; };`
  - `bool matchesFilter(PersistentStorage& storage, Id id, const FilterOptions& options, const std::wstring& displayedDefinitionPath);`

- [ ] **Step 1: Move the helpers** into `StTestSupport.h` and build `Sourcetrail_st_test` with both test files. Expected: every existing test passes unchanged.
- [ ] **Step 2: Write the failing tests** in `StQueryTest.cpp`:
  - `st filter keeps symbols carrying the annotation or inside an annotated type`: class `Ctl` annotated `example.web.Endpoint` with member `get()`; method `Other.run()` without annotation. `annotation = L"Endpoint"` → `Ctl` and `Ctl.get()` match, `Other.run()` doesn't. `L"example.web.Endpoint"` matches too, and `L"point"` doesn't.
  - `st filter combines criteria and --not negates them`: `kinds = NODE_METHOD`, `package = L"p"` → methods match and classes don't. `invert = true` flips each answer.
  - `st filter matches file patterns against the name or the displayed path`: `L"*Test.java"` matches `src/ATest.java`. `L"src/*"` matches `src/ATest.java` and doesn't match `lib/A.java`.
  - `st filter keeps every passing input line unchanged`: `readLines` of two `refs` lines for one key plus `"+3 more (--limit)"` → two `InputLine`s with their raw text intact and `truncated == 3`.
- [ ] **Step 3: Run them to see them fail.** `build/Release/test/Sourcetrail_st_test "st filter*"`.
- [ ] **Step 4: Implement `matchesFilter`.**
  - Find the enclosing types from the name hierarchy's prefixes, resolved with `getNodeIdForNameHierarchy` and kept when their kind is a type.
  - Find annotations with `getGraphForTrail(candidate, 0, all kinds, EDGE_ANNOTATION_USAGE, true, 1, true)` per candidate.
  - Wildcards: `QRegExp(pattern, Qt::CaseSensitive, QRegExp::WildcardUnix).exactMatch`.
- [ ] **Step 5: Wire up `st filter` in `StMain.cpp`.**
  - Add the options `--package`, `--annotated`, `--file` and `--not` (usage error on other commands). `--kind` is reused.
  - Stdin goes through `readLines`. Each key is resolved exactly; unknown keys are reported on stderr and skipped.
  - The displayed definition path comes from `definitionLines` → `mapLocation` → `displayPath`.
  - Print the passing raw lines in order, truncated at `--limit` with `+N more (--limit)`. stderr gets `filter: <n>`. Exit 1 when nothing passes.
  - Update the help text and the usage line.
- [ ] **Step 6: Run all `Sourcetrail_st_test` and `ctest`** in `build/Release`. Expected: everything passes.
- [ ] **Step 7: Commit** `feat(st): filter symbol sets by kind, package, annotation and file`.

### Task 4: `symbols-in` and `at`

**Files:**
- Create: `src/cli/st/StFiles.h`, `src/cli/st/StFiles.cpp`
- Modify: `src/cli/st/StLombok.{h,cpp}`, `src/cli/st/StMain.cpp`, `CMakeLists.txt`
- Test: `src/cli/st/StQueryTest.cpp`

**Interfaces:**
- Consumes: `Code::scope` (Task 3), `LombokRoots`, `alignLines`.
- Produces:
  - `std::vector<Id> definedInFile(PersistentStorage& storage, const FilePath& indexedFile);`: explicit definitions whose definition line is in this file, excluding `NODE_PACKAGE | NODE_FILE | NODE_TYPE_PARAMETER`.
  - `Id symbolAt(PersistentStorage& storage, const FilePath& indexedFile, size_t line);`: returns 0 when there is none.
  - `std::vector<FilePath> matchIndexedFiles(PersistentStorage& storage, const std::wstring& path, const FilePath& cwd);`
  - `std::optional<FilePath> toGeneratedFile(const FilePath& original, const LombokRoots& roots);`: nullopt when the file isn't under an original root, or when its relative path exists under more than one delombok root.
  - `std::optional<size_t> toGeneratedLine(const FilePath& original, const FilePath& generated, size_t line, const std::function<std::vector<std::string>(const FilePath&)>& readLines);`
  - `std::optional<FilePath> toOriginalFile(const FilePath& generated, const LombokRoots& roots);` (the path half of `toOriginal`, factored out).

- [ ] **Step 1: Write the failing tests:**
  - `st symbols-in lists symbols defined in a file, not ones it references`: `A.java` defines `A`, `A.run()` and field `A.x`, and calls `B.go()` → `{A, A.run(), A.x}` in line order.
  - `st at picks the innermost scope containing the line`: class `A` scope 1–20, `A.run()` scope 5–9 → line 7 gives `A.run()`, line 12 gives `A`, line 30 gives 0.
  - `st at lists both files when a bare name is ambiguous`: indexed `/r/a/Util.java` and `/r/b/Util.java` → `matchIndexedFiles(L"Util.java")` returns both. `L"a/Util.java"` returns one.
  - `st at maps an original lombok line to the generated file`: original lines `{"@Getter", "class P {", "int x;", "}"}`, generated `{"class P {", "int x;", "public int getX() {", "}", "}"}` → original line 3 maps to generated line 2.
  - `st at refuses an original line without a generated counterpart`: original line 1 (`@Getter`) → nullopt.
- [ ] **Step 2: Run them to see them fail.** `build/Release/test/Sourcetrail_st_test "st symbols-in*,st at*"`.
- [ ] **Step 3: Implement** with `getSourceLocationsForFile` and `getFileInfoForAllFiles`. `toGeneratedLine` inverts `alignLines(generated, original)`.
- [ ] **Step 4: Wire up in `StMain.cpp`:**
  - `st symbols-in [file...]` (stdin: one path per line) and `st at <file>:<line>` (stdin: `path:line[:...]`).
  - Input paths go through `toGeneratedFile` first. Lines go through `toGeneratedLine` when the file was mapped; with no counterpart, exit 3 with the Global Constraints message.
  - Unknown files: stderr `st: not indexed: <path>`, then skip.
  - Ambiguous bare names: exit 2 with the candidates on stderr.
  - Output is `definitionLines` of each symbol through `printResults`, so Lombok locations map back.
  - stderr gets `symbols-in: <n>` or `at: <n>`.
- [ ] **Step 5: Run all `Sourcetrail_st_test` and `ctest`.**
- [ ] **Step 6: Commit** `feat(st): symbols-in and at turn files and lines into symbols`.

### Task 5: `outline`

**Files:**
- Modify: `src/cli/st/StFiles.{h,cpp}`, `src/cli/st/StOutput.{h,cpp}` (`endLine`), `src/cli/st/StLombok.cpp` (map `endLine`), `src/lib/data/storage/PersistentStorage.{h,cpp}`, `src/cli/st/StMain.cpp`
- Test: `src/cli/st/StQueryTest.cpp`

**Interfaces:**
- Consumes: `definedInFile`, `toGeneratedFile` (Task 4), `Code::access` (Task 3).
- Produces:
  - `AccessKind PersistentStorage::getAccessKindForNodeId(Id nodeId) const;`: `ACCESS_NONE` when no access is stored.
  - `std::vector<ResultLine> outline(PersistentStorage& storage, const std::vector<Id>& symbols);`: one line per symbol, with `endLine` from the scope location and kind `<access> <kind>`.
  - `std::vector<Id> directMembers(PersistentStorage& storage, Id type);`

- [ ] **Step 1: Write the failing tests:**
  - `st outline shows visibility and line ranges`: `A.run()` public with scope 5–9 → kind `L"public method"`, `line == 5`, `endLine == 9`. A symbol without access gets kind `L"method"`.
  - `st outline of a class lists direct members only`: `A` has `run()` and nested `A.In`, and `A.In` has `go()` → members are `{run(), In}`.
  - `st output prints a line range`: `printResults` of a line with `line 5, endLine 9` prints `…:5-9\t…`.
  - `lombok range end without a counterpart is dropped`: `toOriginal` of a range whose end line is unmatched → `endLine == 0`, and the start is still mapped.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.** `getAccessKindForNodeId` wraps `getComponentAccessByNodeId`.
- [ ] **Step 4: Wire up `st outline [file-or-symbol...]`** with the input rule from Global Constraints. For a symbol, list its `directMembers` (a non-type symbol outlines itself). For a file, list `definedInFile`. stderr gets `outline: <n>`.
- [ ] **Step 5: Run all `Sourcetrail_st_test` and `ctest`.**
- [ ] **Step 6: Commit** `feat(st): outline files and classes with visibility and line ranges`.

### Task 6: `impact` and `--edges`

**Files:**
- Create: `src/cli/st/StArchitecture.h`, `src/cli/st/StArchitecture.cpp`
- Modify: `src/lib/data/storage/PersistentStorage.{h,cpp}`, `src/cli/st/StGraph.{h,cpp}` (`paths` takes a type mask), `src/cli/st/StMain.cpp`, `CMakeLists.txt`
- Test: `src/cli/st/StQueryTest.cpp`

**Interfaces:**
- Produces:
  - `std::vector<StorageEdge> PersistentStorage::getEdgesForSourceIds(const std::vector<Id>& ids) const;` and `getEdgesForTargetIds(...)`, which wrap the SQLite queries of the same name.
  - `Edge::TypeMask parseEdges(const std::wstring& list);`: throws `std::invalid_argument` whose `what()` names the unknown entry.
  - `const Edge::TypeMask PATH_EDGES`, `IMPACT_EDGES` (defaults from the Decisions section).
  - `enum class Group { None, File, Package };`
  - `struct ImpactResult { std::vector<ResultLine> lines; size_t symbols; size_t files; size_t packages; };`
  - `ImpactResult impact(PersistentStorage& storage, const std::vector<Id>& symbols, size_t depth, Edge::TypeMask types, Group group);`
  - `paths(storage, from, to, depth, limit, Edge::TypeMask types)` (the new last parameter).

- [ ] **Step 1: Write the failing tests:**
  - `st impact reaches callers through interfaces and subtypes`: `Impl.run()` overrides `Api.run()`. `Client.use()` calls `Api.run()`. `Sub` inherits `Impl`. `Holder.field` has type usage of `Impl`. Then:
    - impact of `Impl.run()` ⊇ `{Api.run(), Client.use()}`;
    - impact of the class `Impl` ⊇ `{Sub, Holder.field, Client.use()}`;
    - impact of `Api.run()` ⊇ `{Impl.run()}`.
  - `st impact honours depth with free override hops`: chain `A.a() → B.b() → C.c()` through calls. Impact of `C.c()` with depth 1 is `{B.b()}`. Depth 0 gives `{A.a(), B.b()}`.
  - `st impact terminates on cycles and leaves out its input`: `A.a()` calls `B.b()`, which calls `A.a()` → impact of `A.a()` is `{B.b()}`.
  - `st impact groups by package`: the `Group::Package` line keys are package names, and the kind starts with `L"package, "`.
  - `st edges parse names and reject unknown ones`: `parseEdges(L"call,override") == (EDGE_CALL | EDGE_OVERRIDE)`. `parseEdges(L"call,calls")` throws with `"calls"` in `what()`.
  - `st path over inheritance only when asked`: a chain that needs an inheritance hop is found with `PATH_EDGES | EDGE_INHERITANCE` and not with `PATH_EDGES`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement `impact`** as a level-batched 0-1 BFS.
  - Each level does one `getEdgesForTargetIds` call over the frontier: users, subtypes and overriders. It also does one `getEdgesForSourceIds` call filtered to `EDGE_OVERRIDE`: the overridden methods.
  - Apply the cost-0 closure before moving to the next level.
  - Drop external symbols (`getDefinitionKindForNodeId`), files, packages and the inputs.
  - Site: the first start location of the reaching link (reuse `linkSite`; move it to a shared internal header if needed).
- [ ] **Step 4: Wire up `st impact [symbol] [--depth N] [--edges L] [--group file|package]`**, plus `--edges` for `path`. An unknown `--group` value or a bad `--edges` list exits 64. Print the stderr summary line from the Decisions section.
- [ ] **Step 5: Run all `Sourcetrail_st_test` and `ctest`.**
- [ ] **Step 6: Measure on shop**, in the scratch copy with `.git` (see `wisdom.md`). Run `time st impact <a widely used service class>` and `time st path <controller> <repository> --depth 6`. Target: impact under 1 s and path under 2 s once the index is fresh. Report both numbers. Report misses rather than tuning blindly.
- [ ] **Step 7: Commit** `feat(st): impact of a change and --edges for path and impact`.

### Task 7: `deps`

**Files:**
- Modify: `src/cli/st/StArchitecture.{h,cpp}`, `src/cli/st/StMain.cpp`
- Test: `src/cli/st/StQueryTest.cpp`

**Interfaces:**
- Consumes: `getEdgesForSourceIds` / `getEdgesForTargetIds`, `Direction` (StGraph.h).
- Produces: `std::vector<ResultLine> deps(PersistentStorage& storage, const std::vector<Id>& symbols, Direction direction);`

- [ ] **Step 1: Write the failing tests.** The `Code` builder needs a package level for these. Add `Id package(const std::wstring& name)` and a `type` overload that takes a package, with member links package → type, the way Sourcetrail's Java indexer records name hierarchies.
  - `st deps rolls up to top-level classes and counts links`: `web.Ctl.get()` calls `domain.Svc.a()` twice (two link sites) and uses `domain.Svc.In` → the `Outgoing` deps of `web.Ctl` are a single line keyed `domain.Svc` with kind `L"class, 3 links"`.
  - `st deps of a package rolls up to packages`: the deps of package `web` are a single line keyed `domain`, with a kind starting with `L"package, "`.
  - `st deps ignores links inside the input`: `Svc.a()` calls `Svc.b()` → the deps of `Svc` are empty.
  - `st deps incoming lists the dependants`: the `Incoming` deps of `domain.Svc` are a line keyed `web.Ctl`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
  - Collect the input's members recursively through member links.
  - Batch `getEdgesForSourceIds` (out) or `getEdgesForTargetIds` (in), excluding member, import and include.
  - Map every other endpoint to its top-level type: the outermost name-hierarchy prefix whose node is a type. For package level, map it to the nearest package ancestor.
  - Drop endpoints inside the input and external ones.
- [ ] **Step 4: Wire up `st deps [symbol] [--in]`.** stderr gets `deps: <n>`.
- [ ] **Step 5: Run all `Sourcetrail_st_test` and `ctest`.**
- [ ] **Step 6: Commit** `feat(st): deps rolls dependencies up to classes or packages`.

### Task 8: End-to-end smoke, fixture, README

**Files:**
- Create: `testing/cli/lombok/src/main/java/example/web/Endpoint.java`, `testing/cli/lombok/src/main/java/example/web/GreetingController.java`
- Modify: `testing/cli/st-smoke.sh`, `testing/cli/smoke.sh` (`completed_files == 7`), `README.md`, `src/cli/st/StMain.cpp` (`status` wording)

- [ ] **Step 1: Add the fixture files** (see Decisions). Run `bash testing/cli/smoke.sh build/Release/app/Sourcetrail_cli` with `JAVA_HOME=~/.jdks/temurin-21.0.3` and update the file count. Expected: `CLI smoke passed`.
- [ ] **Step 2: Extend `st-smoke.sh`** with one assertion per command against the fixture:
  - `st at` on `Caller.java` at a line inside a method prints that method.
  - `st symbols-in` on the original `Person.java` path prints `example.Person` at an original line.
  - `st outline example.Caller` prints a `public method` line with a range.
  - `st impact example.Person.getName()` includes the `Caller` method.
  - `st deps example.web.GreetingController` prints `example.Caller` with `class, `.
  - `st find GreetingController | st callees | st filter --package example --not --kind field` exits 0.
  - `st filter --annotated Endpoint` keeps the controller.
  - `--edges calls` exits 64.
- [ ] **Step 3: Fix the two parked Phase 1 items:**
  - `st status` prints `...; next query refreshes` instead of `...; refreshing`.
  - The README `st` section mentions the `input truncated upstream` stderr line.
- [ ] **Step 4: Update the README `st` section** with the six commands, `--edges`, the input rules (paths, `file:line`, the Lombok original-path rule) and two pipeline examples from the spec.
- [ ] **Step 5: Run everything:** `ctest`, the full `Sourcetrail_test` (compared against the baseline), both smoke scripts.
- [ ] **Step 6: Commit** `test(st): phase 2 smoke test, fixture package and README`.
