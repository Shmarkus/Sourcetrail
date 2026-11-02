# Upgrade port: the fork on a modern build base

Date: 2026-11-02. Status: draft for approval.

## Why

The fork builds on a 2023 toolchain: Qt 5 from apt in a hand-made Docker image (`codehouseou/sourcetrail-builder`),
C++17, no package manager. Getting Qt into that image was painful, and every tool upgrade means rebuilding it by hand.
OpenSourceSourceTrail (remote `upstream`, archived) moved to Conan 2, CMake presets, C++20 and Qt 6.8, but it removed
Java, which is the only language this fork is used for.

The goal is to **upgrade the build tooling easily from now on**, without losing anything the fork can do today.
Success: a branch that builds with Conan + Qt 6 + C++20, has every feature master has now except Python (phase 2), passes the same tests and
benchmarks, and then **replaces master**.

## Goals

1. Base: upstream commit `3e090d9b5` (2026-01-18). This is the last commit before upstream split the engine from the
   GUI (gRPC, HTTP daemon, plugin indexers). The split is out of scope.
2. Carry across, behaving as today:
   - Java indexing: JNI indexer, `java_indexer` jar, Gradle and Maven source groups, delombok with line mapping.
   - Incremental refresh: the declaration-compare second pass, dependent-file sets in RefreshInfo, DirectorySync,
     skipping VACUUM on small refreshes.
   - The storage additions `st` relies on (`getEdgesForSourceIds/TargetIds`, `getStorageNodesForIds`,
     `getDefinitionKindForNodeId`, `getAccessKindForNodeId`, `buildQueryCaches`).
   - `st`, `st serve`, the watcher and the background refresh, with all their tests.
   - The benchmark harness (`testing/benchmark`) and the smoke tests (`testing/cli`).
   - CI: build, test and package on every push; master refreshes the `latest` prerelease in place; `v*` tags publish
     releases. The package ships the GUI, `sourcetrail_indexer` and `st` with all needed libraries bundled.
3. New Java project setup in the GUI (below).
4. `st` finds projects stored in the new `.sourcetrail/` folder.

## Non-goals

- The engine/GUI split, gRPC, the HTTP daemon, the MCP server and upstream's `feature/java-indexer` branch.
- The JSON CLI `Sourcetrail_cli`. `st` does not need the executable. The query code it uses (`src/cli/Query.cpp`,
  `References.cpp`: `reindex`, `prepareIndexer` and the lookups) moves into `st`'s own sources. `Sourcetrail index`
  remains the headless indexer that `st` launches. Its users change as follows (my call):
  - `st` gets a public `st index [--full]` command that runs the same reindex `st` already uses internally;
  - `testing/cli/smoke.sh` (which tests only `Sourcetrail_cli`) is deleted;
  - `st-smoke.sh`, `bench.py` and the benchmark README use `st index` and stop requiring `Sourcetrail_cli`;
  - the README and the release notes stop mentioning it.
- The old Java project wizard pages, which make you pick Gradle or Maven and a project file location.
- Python, in this phase. Upstream removed it along with Java. The user wants it kept, ported as a **second phase**
  once this port has replaced master, by the same method as Java (re-add `lib_python` and the Python source group at
  the language-package seams, with its tests). It gets its own design. The switch does not wait for it (my call): the
  new master ships without Python until phase 2 lands, and the release notes say so.
- The C++ indexer. It stays off, as now.
- Windows and macOS builds. Linux only, as now.
- Following upstream after `3e090d9b5`. The new base is a permanent fork; fixes after that point are backported by hand
  only if needed.

## Anti-goals (failure even if it builds)

- Any change in what an index contains for the same project. Check: identical node, edge and location sets on old and
  new builds. This covers `testing/cli/lombok` (the spike already matched it: 47 nodes, 84 edges, 182 locations), a
  small Maven fixture added to `testing/cli` (there is none yet), the benchmark's `realworld` project, and a scratch
  copy of the private Spring service from the README numbers. Gradle and Maven classpath resolution is where drift is
  most likely, so the Maven fixture and the real projects matter more than Lombok alone. (Fixture set is my call.)
- `st` getting slower or less correct. Check (switch gate, my call):
  - agent benchmark, one run of 9 questions × 3 runs per arm: correctness at least 27/27 as on master, tokens and
    calls within 10% of master's run on the same day;
  - `bench.py speed` on the private service copy: median and slowest query within 10% of master measured the same
    day, after-edit no slower.
  The private service stays the reference; it is not added to the repo.
- Incremental refresh silently re-indexing too little. Check: the fork's storage and refresh tests are ported to gtest.
  Tests that guard behaviour the base already has go in first and must pass on the base. Tests for the fork's own
  behaviour (declaration-compare pass, dependent-file sets, interrupted-refresh fix) land in the same commit as the code
  they cover. (My call: this keeps every commit green.)
- Private project, package or class names in committed files (the repo is public).

## Constraints

- Treat this as a **re-application, not a merge**. Since the shared history (2023-01-30), upstream rewrote and
  reformatted every lib-core file the fork touches, so a merge conflicts on every hunk. Port each change from its intent.
- **Port the tests first.** The fork's tests (Catch2) move to upstream's gtest (`add_sourcetrail_test`) before the code
  they cover.
- Upstream's `LOG_*` macros treat their first argument as an fmt format string. Every ported call with dynamic text
  becomes `LOG_X("{}", text)`, otherwise a `{` in a path or in Gradle output throws.
- Keep and own the base fixes found by the spike:
  - the `ApplicationArchitectureType::ARM` enum value;
  - headless `Sourcetrail index`: argc by reference, the message queue created before listeners, and argv copied
    correctly;
  - the toolchain profile uses gcc 13 + lld, not mold.
- Existing indexes are not carried over (my call). Master writes storage version 26, the base is at 25. The new build
  uses **27**, so an old database is rejected cleanly and reindexed in full once, instead of being misread. Existing
  `.srctrlprj` files, `~/.config/sourcetrail/ApplicationSettings.xml` (JDK path) and `st`'s project registry keep
  working; if upstream moved or renamed any of them, the new build reads the old location.
- Pin dependencies with a Conan lockfile. Cache `CONAN_HOME` and Qt in CI (about 3.2 GB).
- Development happens on a branch with its own worktree. master keeps working and shipping until the switch.
- Never modify the real projects under `~/Projects`; test on copies.

## Starting point

The spike (local branch `spike/upstream-jan2026`, commit `550eb3614`, report in the session scratchpad) proved:
- the base builds here (Conan install 186 s cold, build 181 s), and all 578 upstream tests pass when run serially;
- the fork's `lib_java` compiles on the base after about 10 edits, with zero Qt 6 or C++20 errors;
- Lombok fixture indexes are identical, in single- and multi-process mode.

The spike commit can serve as a starting point for the Java part. It carries `// spike:` markers and shortcuts (copied
utilities in `lib/utility/java_port`, a force-included logging shim, jars reused rather than built) that the real port
replaces.

## GUI project setup for Java (new behaviour)

Today you pick Gradle or Maven, then pick where the Sourcetrail project file goes. New flow:

1. **New Project → pick a folder.** Nothing else is required.
2. Sourcetrail looks in that folder:
   - `settings.gradle(.kts)` or `build.gradle(.kts)` → Gradle source group;
   - `pom.xml` → Maven source group;
   - both → Gradle (my call; a Gradle build that also has a pom is usually migrating away from Maven);
   - neither → a message saying no Gradle or Maven build was found, with two choices: a plain Java project with a
     classpath you fill in, or cancel.
3. Everything Sourcetrail writes goes into **`<project>/.sourcetrail/`**: the `.srctrlprj`, the database, the delombok
   output, Gradle/Maven caches and the logs. The project is named after the folder.
4. `.sourcetrail/` gets its own `.gitignore` containing `*`, so it never shows up in git and the user's own
   `.gitignore` is never touched.
5. Opening a folder that already has `.sourcetrail/*.srctrlprj` opens that project instead of creating a second one.
6. Defaults: tests are indexed (`st`'s "which tests reach this" needs them); the JDK comes from the Sourcetrail
   settings, as now. After creating the project, indexing starts as it does today.

The user chose a dot folder over the build tool's `build/` or `target/` folder, because `clean` would delete the index.

Existing projects with the file elsewhere keep working: they open from the GUI as before, and `st` reaches them
through `--project` and its registry.

Left to the builder: whether the detection lives in a new start-screen action or replaces the source-group selection
page in the existing wizard, and how the settings page for an existing project looks. The old Gradle/Maven pages may
be reused internally for editing settings, but creating a project must not require them.

## `st` project discovery

Today `st` walks up from the current folder looking for a `*.srctrlprj`, then checks its registry at each level. New:
at each level it also checks `.sourcetrail/*.srctrlprj`. Search order at one level: a `.srctrlprj` in the folder
itself, then one in `.sourcetrail/`, then the registry. `st`'s own working files (`.st-serve.log`, `.st-refresh.log`,
the `.srctrlst` freshness files) go next to the project file, as now, so they land in `.sourcetrail/` for new projects.

The file watcher must ignore `.sourcetrail/`, as it already ignores build output.

## Order of work (from the spike's estimate, about 3–5 focused weeks)

1. **Base**: `3e090d9b5` + the base fixes, own preset, lockfile, gcc 13 + lld profile. Local build and tests green.
2. **Java**: productionise the spike's Java port. Build the jar from CMake and stage it; proper `core::utility` targets
   instead of `java_port`; ProjectSettings Java migrations and the Java bookmark separator; the fmt-safe logging audit;
   the Java tests in gtest. Fixture indexes identical to master.
3. **Lib-core features**: port the storage and refresh tests, then re-apply the incremental refresh, storage
   additions, VACUUM skip and delombok JVM flags.
4. **`st`**: move it over with its tests (Catch2 → gtest). Fold the needed query code into `st`. Replace `QRegExp` and
   `SkipEmptyParts` with their Qt 6 forms. Add `.sourcetrail/` discovery and `st index`. Move `st-smoke.sh` and the
   benchmark harness to `st index`. Smoke tests pass.
5. **GUI**: the new Java project setup above, plus the Java path detectors (JDK, Maven) on upstream's detector layout.
6. **CI and packaging**: Conan + aqt (or a new builder image) with caching, Qt 6 library and plugin bundling, the same
   release flow as now, smoke tests against the packaged binaries.
7. **Switch**: run the benchmark and speed checks against the new build, compare with master, update `README.md`, then
   make the branch the new master.

Each step ends with a green build and tests, so the branch is never broken for long.

## Left to the builder

- Whether CI uses a new builder image with Conan and Qt preinstalled, or installs them in the job with caching.
- How the new tree's CMake targets are named and laid out, following upstream's `Sourcetrail::…` style.
- Whether `java_indexer` jar staging uses Maven from CMake or a prebuilt artifact, as long as a clean checkout builds
  with one command sequence.
- How master is replaced at the switch (a merge with `-s ours` history or a force-reset), to be agreed with the user at
  that time; force-pushing master needs their explicit go.
