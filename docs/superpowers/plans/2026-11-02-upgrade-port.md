# Upgrade Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the fork on upstream `3e090d9b5` (Conan 2, CMake presets, C++20, Qt 6.8) with Java indexing, incremental refresh, `st`, the new Java project setup and CI, ready to replace master.

**Architecture:** A new branch `upgrade/modern-base` starts at upstream `3e090d9b5`, not at master. Fork features are re-applied by intent, one area per task, each with its tests ported to gtest first. Master's code is the reference: every port task names the master commits whose behaviour it carries, and `git show <commit>` is how the implementer reads the intent. Index output is compared against master's binary with one script throughout.

**Tech Stack:** C++20, Qt 6.8.2 (aqt), Conan 2 + lockfile, CMake presets, gcc 13 + lld, gtest/gmock, JNI + the Maven-built `java_indexer` jar, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-02-upgrade-port-design.md`

## Global Constraints

- Base: upstream commit `3e090d9b5`. Nothing from upstream after it (engine split, gRPC, MCP, `feature/java-indexer`).
- Re-application, not merge: never `git merge` master or upstream into the branch. Port from `git show <master commit>`.
- Tests first: a task's ported tests (Catch2 → gtest via `add_sourcetrail_test`) go in before or with the code they cover; every commit builds and passes `ctest --preset fork_release`.
- Logging: every `LOG_*` with dynamic text is `LOG_X("{}", text)`; no string concatenation as the first argument.
- Index storage version: **27** (master is 26, base 25). Old databases are rejected and fully reindexed.
- Project files of new Java projects live in `<project>/.sourcetrail/`, which contains a `.gitignore` with the single line `*`.
- `Sourcetrail_cli` is not built or shipped. `st index [--full]` replaces its indexing role.
- Python is out of scope (phase 2). Do not build or ship `data/python`; release notes say Python returns in a later release.
- C++ indexer off. Linux only.
- Toolchain: gcc 13, lld (`USE_ALTERNATE_LINKER=lld`), Qt 6.8.2 from aqt, Conan dependencies pinned by `conan.lock`.
- Format new and ported C++ with the repo's `.clang-format`.
- Never touch the real projects under `~/Projects`; test on copies under the scratchpad.
- Repo is public: no private project, package or class names in files or commit messages; use neutral names (`com.acme.shop`, "a private 2k-file Spring service").
- Commits end with the two attribution lines from the session. Never `git add -A`, never `git stash`. No push or merge without the user asking.
- Run the app and tests with `HOME` pointed at a scratch directory, so `~/.config/sourcetrail` is never written.

## Review Focus

1. A path, class name or Gradle/Maven output containing `{` or `}` reaching a log call: indexing must log it and carry on, never throw `fmt::format_error`. Test in Task 2.
2. A database written by master (version 26) opened by the new build, from the GUI or `st`: must be recognised as outdated and fully reindexed, never read. Test in Task 4.
3. A folder with both `pom.xml` and `build.gradle`, or only `build.gradle.kts`, or only `settings.gradle`: must give Gradle; only `pom.xml` gives Maven; nested module build files without a root one give "none found". Test in Task 8.
4. A tree with both a root-level `x.srctrlprj` and `.sourcetrail/y.srctrlprj`, and `st` run from a symlinked path or from inside `.sourcetrail/`: the root-level file wins, and the symlinked path resolves the same project. Test in Task 7.
5. The indexer writing its database inside the watched project (`.sourcetrail/`): the watcher must not see those writes as source changes and must not loop refreshing. Test in Task 7.

---

## Toolchain and commands used by every task

Task 1 creates these; later tasks use them verbatim.

- `scripts/fork/bootstrap.sh`: creates `$ST_TOOLCHAIN` (default `$HOME/.cache/sourcetrail-toolchain`) with a venv holding conan and aqtinstall, Qt 6.8.2 `linux_gcc_64` with modules `qt5compat qtimageformats`, and `CONAN_HOME`; then runs
  `conan install . -s build_type=Release -of .conan/gcc/ -b missing -pr:a .conan/gcc/profile --lockfile conan.lock`.
  Prints the `export` lines for `CONAN_HOME`, `CMAKE_PREFIX_PATH` and `PATH`. It must be idempotent.
- Configure, build, test:
  ```bash
  eval "$(scripts/fork/bootstrap.sh --env)"
  cmake --preset fork_release
  cmake --build --preset fork_release -j16
  HOME=$SCRATCH/home ctest --preset fork_release
  ```
  `fork_release` (configure, build and test presets) inherits `ci_gnu_release`, sets `binaryDir` to `build/fork-release`, `USE_ALTERNATE_LINKER=lld`, `BUILD_JAVA_LANGUAGE_PACKAGE=ON`, and runs tests serially (upstream's integration suites share temp databases and fail in parallel).
- Master reference binaries: build master once in a separate worktree (`build/Release` there) and keep its path as `$MASTER_APP`.
- `testing/port/compare-index.sh <old app dir> <new app dir> <project dir>` (Task 2): indexes a copy of the project with each build and diffs the sorted node names, edges `(type, source name, target name)` and source locations `(file relative to project, start, end, type)`. Exit 0 and prints `identical` when equal.

---

### Task 1: Branch, base fixes and toolchain

**Files:**
- Branch `upgrade/modern-base` from `3e090d9b5`, in a worktree.
- Modify: `.conan/gcc/profile` (gcc `compiler.version=13`, drop `LD=mold`), `CMakePresets.json` (add `fork_release`).
- Create: `conan.lock`, `scripts/fork/bootstrap.sh`.
- Modify (base fixes, carried from spike commit `550eb3614`, Part A): `src/lib_gui/utility/utilityApp/utilityApp.h` (add `ARM` to `ApplicationArchitectureType`), `src/lib_gui/qt/QtCoreApplication.{h,cpp}` (argc by `int&`), the console path in `src/app/main.cpp` (message queue before listeners; `std::vector<std::string>(argv + 1, argv + argc)`).
- Replace the `bin` submodule with a tracked `bin/` tree: the submodule's content at its pinned commit, committed into the repo, `.gitmodules` entry removed. (My call: the fork must own its test data and Java resources.)
- Copy unchanged from master: `README.md`, `docs/`, `testing/` (the code that uses them is fixed in later tasks; README is rewritten in Task 10).
- Test: `src/app/tests/HeadlessConsoleTestSuite.cpp` (new, gtest).

**Interfaces:**
- Produces: preset `fork_release`; `scripts/fork/bootstrap.sh [--env]`; a working headless `Sourcetrail index|config` used by Tasks 2–7.

- [ ] **Step 1:** Create the worktree and branch from `3e090d9b5`; init the `bin` submodule, then convert it to tracked files in one commit.
- [ ] **Step 2:** Write `bootstrap.sh`, the profile change and the preset; run bootstrap; generate `conan.lock` with `conan lock create`.
- [ ] **Step 3: Write the failing test.** `HeadlessConsoleTestSuite`, registered with `add_sourcetrail_test(NAME headless_console_test ...)`:
  - `TEST(HeadlessConsole, configPrintsHelpAndExitsZero)`: runs the built `Sourcetrail config --help` through `QProcess` with a scratch `HOME`; expects exit code 0 and output containing `config`.
  - `TEST(HeadlessConsole, argumentsArePassedInOrder)`: runs `Sourcetrail index --help --full`; expects exit 0 (before the argv fix every slot repeats `index`).
- [ ] **Step 4:** Build and run; expect both to fail (SIGSEGV / wrong arguments) on the unfixed base.
- [ ] **Step 5:** Apply the four base fixes. Build; `ctest --preset fork_release` passes in full (578 upstream tests + 2 new).
- [ ] **Step 6:** Copy `README.md`, `docs/`, `testing/` from master. Commit in three commits: submodule conversion; toolchain/preset/lockfile; base fixes with the test.

### Task 2: Java language package

**Files:**
- Start from spike commit `550eb3614` (Part B): `src/lib_java/**`, `src/lib/settings/source_group/component/java/*`, `SourceGroupSettingsJava{Empty,Gradle,Maven}.h`, the `BUILD_JAVA_LANGUAGE_PACKAGE` hook blocks, `src/external/tinyxml`.
- Replace spike shortcuts: `src/lib/utility/java_port/` → `src/core/utility/xml/` (`utilityXml`) and `src/core/utility/library/` (`utilityLibrary`, `types.h`), each its own `Sourcetrail::core::utility::…` target; delete `lib_java/javaPortCompat.h` and fix every `LOG_*_STREAM` site directly.
- Create: `cmake/java_indexer.cmake` — a custom target `java_indexer_jar` running `mvn -q -f java_indexer/pom.xml package` and staging `java-indexer.jar` plus its dependency jars into `bin/app/data/java/lib`; the `Sourcetrail` target depends on it. Copy `java_indexer/` and `script/update_java_indexer.sh` behaviour from master.
- Modify: `src/lib/settings/ProjectSettings.cpp` (master's Java migrations and the `languageName "java"` block), `src/lib/data/storage/sqlite/SqliteBookmarkStorage.cpp` (Java name separator), `SqliteIndexStorage` storage version → 27.
- Create: `testing/port/compare-index.sh`, `testing/cli/maven/` (small Maven fixture: two modules, one depending on the other, one external dependency resolved from `~/.m2`, one test class).
- Tests: `src/lib_java/tests/` — gtest ports of master's `src/test/{UtilityGradle,UtilityMaven,GradleCache,JavaParser,JavaIndexSampleProjects}TestSuite.cpp`, using the same data under `bin/test/data/<Suite>` (copy from master).

**Interfaces:**
- Produces: `Sourcetrail::lib_java`; `java_indexer_jar` target; `compare-index.sh`; storage version 27.

- [ ] **Step 1:** Bring the spike's Part B files onto the branch; build green with `BUILD_JAVA_LANGUAGE_PACKAGE=ON`.
- [ ] **Step 2: Port the Java tests** to gtest, one `TEST` per Catch2 `TEST_CASE`, same names in CamelCase, same assertions. Build; they pass (they cover behaviour the spike already carries) except anything needing the parts in Step 4, which must fail.
- [ ] **Step 3: Write the failing test** for Review Focus 1: `TEST(JavaIndexing, bracesInPathsAndOutputAreLoggedNotThrown)` in `src/lib_java/tests/JavaLoggingTestSuite.cpp`: copy `testing/cli/lombok` into a scratch dir named `with{braces}`, index it headless; expect 0 errors and the indexed file count of the fixture (9).
- [ ] **Step 4:** Replace the `java_port` utilities with core targets, remove the logging shim, audit every `LOG_*` in `src/lib_java` and the Java settings code to the `("{}", text)` form, add ProjectSettings migrations, the bookmark separator, the jar target, version 27. All tests pass.
- [ ] **Step 5:** Write `compare-index.sh` and the Maven fixture (master stays untouched: the script copies each fixture into a scratch dir before indexing with `$MASTER_APP`). Run against `testing/cli/lombok`, `testing/cli/maven` and `testing/benchmark/projects/realworld.srctrlprj`'s project. Expected: `identical` for all three.
- [ ] **Step 6: Settings location.** Add `TEST(UserPaths, applicationSettingsStayWhereMasterKeepsThem)` in `src/lib/tests/UserPathsTestSuite.cpp`: with `HOME=<scratch>`, the application settings file resolves to `<scratch>/.config/sourcetrail/ApplicationSettings.xml` and a master-written file there (copy one from a master run) loads with its JDK path intact. If upstream moved the file, make the new build read the old location.
- [ ] **Step 7:** Commit (tests; core utility targets; Java package; jar target; compare script + fixture; settings location).

### Task 3: Storage and refresh tests that guard base behaviour

**Files:**
- Test: `src/lib/tests/StorageTestSuite.cpp`, `RefreshInfoGeneratorTestSuite.cpp`, `SourceGroupTestSuite.cpp` (new gtest files, ports of master's `src/test/` suites of the same names).
- Test data: `bin/test/data/{StorageTestSuite,RefreshInfoGeneratorTestSuite,SourceGroupTestSuite}` from master.

- [ ] **Step 1:** For each `TEST_CASE` in master's three suites, find the master commit that added it (`git log -S'<test name>' master -- src/test`). Cases added before `6b792d5a4` or not by the commits listed in Tasks 4–5 guard base behaviour: port them here.
- [ ] **Step 2:** Run; all pass on the branch. A failure here is a real base regression: stop and report it, do not adapt the test.
- [ ] **Step 3:** Write the list of the remaining (fork-behaviour) cases and the commit each belongs to into `docs/superpowers/plans/2026-11-02-upgrade-port-tests.md`; Tasks 4 and 5 port them from that list.
- [ ] **Step 4:** Commit.

### Task 4: Storage additions

Carries master commits `4fc7cb353` (find importers through the declaring file), `4cfe85ddf` (clear only what the cleared files alone declare), the storage half of `2d895ca73` (`buildQueryCaches`, library fallback cost), and the storage additions `st` uses.

**Files:**
- Modify: `src/lib/data/storage/PersistentStorage.{h,cpp}`, `src/lib/data/storage/sqlite/SqliteIndexStorage.{h,cpp}`, `SqliteStorage.{h,cpp}`, `src/lib/data/storage/sqlite/CppSQLite3*` if master's change there is needed.
- Test: `src/lib/tests/StorageTestSuite.cpp` (the cases listed for these commits), `src/lib/tests/StorageVersionTestSuite.cpp` (new).

**Interfaces:**
- Produces, on `PersistentStorage`, with master's exact signatures: `getEdgesForSourceIds`, `getEdgesForTargetIds`, `getStorageNodesForIds`, `getDefinitionKindForNodeId`, `getAccessKindForNodeId`, `buildQueryCaches()`.

- [ ] **Step 1:** Port the listed test cases; add `TEST(StorageVersion, version26DatabaseIsReportedIncompatible)`: create a db with `setVersion(26)`, open with `PersistentStorage`; expect `isIncompatible()` true. Run; the fork cases and (if the base check differs) the version test fail.
- [ ] **Step 2:** Re-apply each commit by intent onto the rewritten files. Run; all pass.
- [ ] **Step 3:** Commit, one commit per carried master commit, message naming it (`port: <master subject> (master <sha>)`).

### Task 5: Incremental refresh and Gradle/delombok refresh

Carries, in this order: `044525e80` (trail loop, stale same-package callers), `1000ded3f` (short process timeout), `2e3d9cdd9`, `e4342e0e2`, `517be5a78`, `99b599169`, `da7772dfc` (delombok output kept, sync failure-safe, only changed files, rebuild on full refresh), `fc435efbf`, `14d8f4ea9` (`TaskIndexIfDeclarationsChanged`), `2d7c5439c` (interrupted refresh), `fe81d2806` (VACUUM skip, delombok JVM flags).

**Files:**
- Modify: `src/lib/project/{Project,RefreshInfo,RefreshInfoGenerator,SourceGroup}.*`, `src/lib/data/TaskFinishParsing.cpp`, `src/lib/app/Application.cpp`, `src/lib_java/utility/utilityGradle.cpp`, `src/lib_java/**/DirectorySync*`, the process utility in `src/lib_gui/utility/utilityApp/`.
- Create: `src/lib/data/TaskIndexIfDeclarationsChanged.{h,cpp}`.
- Test: the remaining cases from the Task 3 list in `StorageTestSuite`, `RefreshInfoGeneratorTestSuite`, `SourceGroupTestSuite`, `src/lib_java/tests/DirectorySyncTestSuite.cpp`, the `UtilityGradle`/`GradleCache` cases these commits added.

- [ ] **Step 1:** Per commit: port its test cases, run them and see them fail, re-apply the change, run the full suite green, commit `port: <subject> (master <sha>)`.
- [ ] **Step 2: Refresh identity check.** Extend `compare-index.sh` with `--after-edit <file>`: after the full index, append a method to the given file in both copies, run a non-full refresh, then compare. Run it on `testing/cli/lombok` (edit `example/Person.java`) and `testing/cli/maven`. Expected: `identical`, and the refreshed file count printed by both builds is equal.
- [ ] **Step 3:** Commit the script change.

### Task 6: Move `st` onto the new base

**Files:**
- Create: `src/st/` with master's `src/cli/st/*` (`St*.{h,cpp}`), plus `src/st/StQueries.{h,cpp}` = master's `src/cli/Query.{h,cpp}` and `References.cpp` reduced to what `st` calls (`prepareIndexer`, `reindex`, the lookups). Namespace stays as master's.
- Create: `src/st/CMakeLists.txt` — library `Sourcetrail::st_core`, executable `st`, test target `st_test` via `add_sourcetrail_test`.
- Test: `src/st/tests/StTest.cpp`, `src/st/tests/StQueryTest.cpp` (gtest ports of master's Catch2 files, every case kept), `StTestSupport.h`.

**Interfaces:**
- Consumes: Task 4's storage functions; Task 1's headless `Sourcetrail index`.
- Produces: `st` binary with the same commands, output and exit codes as master.

- [ ] **Step 1:** Port the two test files to gtest (one `TEST` per `TEST_CASE`, `SECTION`s become separate tests or fixtures; assertions unchanged).
- [ ] **Step 2:** Bring the sources over; replace `QRegExp` with `QRegularExpression` and `QString::SkipEmptyParts` with `Qt::SkipEmptyParts`; link against the `Sourcetrail::…` targets instead of master's `--start-group` archives.
- [ ] **Step 3:** Build; all `st_test` cases pass, including master's registry tests (the registry file location and format are unchanged).
- [ ] **Step 4:** Run `testing/cli/st-smoke.sh` against the new `st` with master's `Sourcetrail_cli` copied next to it (temporary; Task 7 removes that need). Expected: passes as on master.
- [ ] **Step 5:** Commit.

### Task 7: `st index`, `.sourcetrail/` discovery, watcher

**Files:**
- Modify: `src/st/StCommand.cpp` (new `index` command), `src/st/StProject.cpp` (discovery), `src/st/StWatch.cpp` (ignore `.sourcetrail/`).
- Modify: `testing/cli/st-smoke.sh` (use `st index`; drop the `Sourcetrail_cli` requirement), `testing/benchmark/bench.py`, `testing/benchmark/README.md`. Delete `testing/cli/smoke.sh`.
- Test: `src/st/tests/StTest.cpp`.

**Interfaces:**
- Produces: `st index [--full]` — runs the same reindex as `st`'s automatic refresh (non-full by default), prints `indexed <n> files, <m> errors` on stdout, exit 0 on success, 1 on indexing failure, 2 on usage errors; goes through `st serve` when one is running, like other commands.
- Produces: discovery order per directory level: `*.srctrlprj` in the directory, then `.sourcetrail/*.srctrlprj`, then the registry.

- [ ] **Step 1: Write the failing tests:**
  - `StIndex.fullIndexReportsFileCount`: on a copy of `testing/cli/lombok`, `st index --full` exits 0 and prints `indexed 9 files, 0 errors`.
  - `StProject.findsProjectInDotFolder`: project at `repo/.sourcetrail/repo.srctrlprj`, cwd `repo/src/main`; finds it.
  - `StProject.rootFileWinsOverDotFolder` (Review Focus 4): both `repo/x.srctrlprj` and `repo/.sourcetrail/y.srctrlprj`; finds `x`.
  - `StProject.symlinkedCwdAndInsideDotFolder` (Review Focus 4): cwd is a symlink to `repo/src`, and separately cwd `repo/.sourcetrail`; both find `repo/.sourcetrail/repo.srctrlprj`.
  - `StWatch.ignoresWritesInsideDotFolder` (Review Focus 5): watcher on a project whose db is in `.sourcetrail/`; writing to `.sourcetrail/repo.srctrldb` for 2 s schedules no refresh; touching a `.java` file schedules one.
- [ ] **Step 2:** Run; they fail.
- [ ] **Step 3:** Implement. Run; all `st_test` pass.
- [ ] **Step 4:** Update `st-smoke.sh`, `bench.py`, the benchmark README; delete `smoke.sh`. Run `st-smoke.sh` with only the new build's binaries; passes.
- [ ] **Step 5:** Commit.

### Task 8: GUI — new Java project from a folder

**Files:**
- Create: `src/lib/project/JavaProjectSetup.{h,cpp}` (no Qt widgets; testable).
- Modify: `src/lib_gui/qt/window/QtStartScreen.{hpp,cpp}` and/or `src/lib_gui/qt/project_wizard/` (the entry point is the builder's choice per spec), plus master's Java wizard pages and the JDK/Maven path detectors (`lib_gui/utility/path_detector/{java_runtime,maven_executable}`) ported onto upstream's detector layout for the settings editor.
- Test: `src/lib/tests/JavaProjectSetupTestSuite.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  enum class JavaBuildKind { Gradle, Maven, None };
  JavaBuildKind detectJavaBuild(const FilePath& folder);          // top level only
  FilePath projectFileFor(const FilePath& folder);                // <folder>/.sourcetrail/<folder name>.srctrlprj
  FilePath existingProjectFile(const FilePath& folder);           // first .sourcetrail/*.srctrlprj, or empty
  FilePath createJavaProject(const FilePath& folder, JavaBuildKind kind);  // writes settings + .gitignore, returns project file
  ```
  `createJavaProject` writes a Gradle or Maven source group with tests indexed, the JDK taken from the application settings, and `.sourcetrail/.gitignore` containing `*\n`. For `None` the GUI offers a plain Java (empty) source group with an editable classpath, or cancel.

- [ ] **Step 1: Write the failing tests** (Review Focus 3):
  - `detectsGradleFromBuildGradle`, `…FromBuildGradleKts`, `…FromSettingsGradleOnly`, `detectsMavenFromPom`, `gradleWinsWhenBothExist`, `nestedModuleBuildFilesAloneGiveNone`.
  - `createWritesProjectIntoDotFolderWithGitignore`: file at `projectFileFor(folder)`, `.gitignore` content exactly `*\n`, loads back as a Gradle source group with tests included.
  - `existingProjectIsFound`: after create, `existingProjectFile(folder)` returns it.
- [ ] **Step 2:** Run; fail. Implement `JavaProjectSetup`. Run; pass.
- [ ] **Step 3:** Wire the GUI: New Project → folder picker → `existingProjectFile` opens it if present; otherwise detect, create, open and start indexing. Port the detectors and Java settings pages so an existing project's settings can be edited.
- [ ] **Step 4:** Manual check with `HOME=$SCRATCH/home`: create projects from copies of `testing/cli/lombok` and `testing/cli/maven`; both index with 0 errors; reopening the folder opens the same project. Then `st` run inside the copy finds it.
- [ ] **Step 5:** Commit.

### Task 9: CI and packaging

**Files:**
- Modify: `.github/workflows/` — replace upstream's workflows with one `build.yml`: ubuntu-22.04 runner (my call: no container; installs gcc 13, lld, Maven, patchelf; JDK 21 via `actions/setup-java`), caches `$ST_TOOLCHAIN` keyed on `conan.lock` + Qt version, runs `bootstrap.sh`, the preset build, `ctest --preset fork_release`, packaging, `st-smoke.sh` against the packaged `st`; keep master's release job unchanged (rolling `latest` prerelease updated in place on master, `v*` tag releases). Release notes text: `Built from master at $GITHUB_SHA. Includes the GUI and st. Python support returns in a later release.`
- Modify: `setup/Linux/createPackages.sh`, `setup/Linux/bundleLibraries.sh` (from master) for Qt 6: plugin dirs `platforms xcbglintegrations imageformats iconengines platforminputcontexts tls`, bundle `libxcb-cursor.so.0` (Qt 6 xcb needs it, many desktops lack it), find plugins via `qtpaths6 --query QT_INSTALL_PLUGINS` or the aqt path; ship `Sourcetrail`, `sourcetrail_indexer`, `st`, `data/java`; no `Sourcetrail_cli`, no `data/python`.
- Modify: `setup/Linux/data/package/{install.sh,uninstall.sh,README}` — drop `Sourcetrail_cli`.

- [ ] **Step 1:** Package locally; unpack into a clean `ubuntu:24.04` container with only base desktop libs; `ldd` shows nothing missing for every binary and plugin; `st --help` runs; `Sourcetrail --version` runs under `QT_QPA_PLATFORM=offscreen`.
- [ ] **Step 2:** Push the branch (user approval needed — ask first) and get a green run on `upgrade/modern-base`; the release job must not run for this branch.
- [ ] **Step 3:** Commit.

### Task 10: Switch gate and docs

**Files:**
- Modify: `README.md` (build instructions → bootstrap + presets; drop `Sourcetrail_cli`; Python note; new project setup; `.sourcetrail/`), `docs/agents/st-claude-md-snippet.md` (`st index`).
- Create: `docs/superpowers/specs/2026-11-02-upgrade-port-results.md`.

- [ ] **Step 1:** `compare-index.sh` (full and `--after-edit`) on a scratch copy of the private Spring service: `identical`.
- [ ] **Step 2:** On the same day, with the API key unset (`env -u ANTHROPIC_API_KEY`) and a warm `st serve` per arm, run the agent benchmark (9 questions × 3 runs per arm) and `bench.py speed` for master and for the branch. Gate: correctness ≥ 27/27; tokens and calls within 10% of master; median and slowest query within 10% of master; after-edit no slower.
- [ ] **Step 3:** Write the results doc with both sets of numbers; update README's measured table with the new build's numbers.
- [ ] **Step 4:** Commit. Then stop and ask the user how to replace master (force-reset vs merge with `-s ours`); do not touch master without their go.
