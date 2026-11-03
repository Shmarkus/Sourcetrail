# TypeScript Indexing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Index a project's TypeScript into the same Sourcetrail database as its Java, link frontend HTTP calls to Spring handlers, and let `st` answer "what does this frontend method reach" and "who on the frontend reaches this backend method" in one command each.

**Architecture:** A Node tool `tools/ts-indexer/` (from the spike's `scip2srctrl.mjs`) runs `scip-typescript` and writes a Sourcetrail sub-database; a new TypeScript source-group type runs it through the existing custom-command machinery, with group-scoped clearing and its own trigger files. A separate linking step (`link.mjs`, run by a new core task after any refresh that touched TypeScript or Spring-mapping files) adds `HTTP.<VERB> <path>` endpoint nodes. `st` gains filters on top of its existing graph commands; the GUI detects `tsconfig.json`.

**Tech Stack:** C++20/Qt 6.8 core and GUI, gtest; Node.js ≥ 20 (`node:test` for the tool's tests), `@sourcegraph/scip-typescript` 0.6.x, `typescript` (AST pass); Python stdlib only under `testing/`.

**Spec:** `docs/superpowers/specs/2026-11-03-typescript-indexing-design.md`

## Global Constraints

- Base: master at v2.0.0-fork; index storage version stays 27; `st` lives in `src/st`; new GUI projects live in `<folder>/.sourcetrail/`.
- Node.js and the project's `node_modules` are runtime prerequisites. Missing Node or `node_modules` → the TypeScript step reports one clear error; the Java part still indexes and the refresh as a whole does not fail.
- A Java-only edit must not run `scip-typescript` or the converter. The linking step may run.
- A `.ts` edit re-indexes only the TypeScript group, never the Java.
- An unresolved or ambiguous URL produces no link and is listed as unresolved; never a guessed link.
- Java nodes/edges/locations of a mixed project are identical to a Java-only index of the same folder, excluding `HTTP.*` endpoint nodes and every link to or from them.
- With no TypeScript in a project, every `st` command's output is unchanged; default `callees`/`supers`/`deps` output is unchanged (library targets only in the boundaries answer or behind `--library`).
- One TypeScript project per folder: the root `tsconfig.json` with its `references`; webapp root from its `baseUrl`/`rootDir`. Multi-app layouts are reported as unsupported.
- No config-based URL prefixes (`server.servlet.context-path`, gateway prefixes).
- Out of scope: Angular HTML templates, routes, cross-index links, following Feign into other services, Python, JS-only projects.
- No private project/package/class names in committed files; the test fixture uses `com.acme.shop` / neutral Angular names.
- Every commit builds and passes `ctest --preset fork_release` (HOME in scratch) and `node --test tools/ts-indexer`; commits end with the session's two attribution lines; never `git add -A`, never `git stash`; push only when the user asks.
- Name delimiter `.`; directories → packages, files → modules named after their stem, npm packages → one package node; module-level code → `<module>.toplevel()`.

## Review Focus

1. A project where `npm install` was never run (no `node_modules`): Java indexes normally, `st status` and the GUI show one clear "TypeScript skipped: run npm install" message, no crash, no half-written TypeScript data. Test in Task 2.
2. Editing a controller's mapping path (`@GetMapping("/a")` → `"/b"`) and refreshing: the old link disappears and a new one appears for any frontend call to `/b`; no stale `HTTP.GET /a` node remains. Test in Task 3.
3. Two Spring handlers matching the same verb and normalised path (e.g. overloads in two controllers): the frontend call is left unlinked and reported ambiguous, never linked to either. Test in Task 3.
4. Deleting a `.ts` file and refreshing: its nodes, its calls and any endpoint links from it are gone; Java data untouched. Test in Task 2.
5. A folder whose `tsconfig.json` sits next to a Maven/Gradle build but whose TypeScript lives in a sub-folder (`src/main/webapp`): names are rooted at the webapp root (no `src.main.webapp.` prefix) and files resolve. Test in Task 1.

---

### Task 1: The TypeScript indexer tool and the shared fixture

**Files:**
- Create: `tools/ts-indexer/package.json` (pins `@sourcegraph/scip-typescript` 0.6.x, `typescript`, `protobufjs` or the SCIP bindings), `tools/ts-indexer/package-lock.json`, `tools/ts-indexer/index.mjs` (CLI), `tools/ts-indexer/convert.mjs` (SCIP → Sourcetrail rows), `tools/ts-indexer/sqlite.mjs` (writer; Node's built-in `node:sqlite` if on Node ≥ 22.5, else `better-sqlite3` — builder's choice, pinned), `tools/ts-indexer/test/*.test.mjs`.
- Create: `testing/cli/angular-spring/` — fixture: a Gradle Spring app (`com.acme.shop`: `OrderResource` with class `@RequestMapping("/api/orders")` and `@GetMapping`, `@PostMapping`, `@GetMapping("/{id}")`; `OrderService` → `OrderRepository extends JpaRepository`; `PaymentClient` `@FeignClient`) plus `src/main/webapp/app/` TypeScript (an `OrderService` making `this.http.get/post` calls with literal, template-string and `resourceUrl`-field URLs; an `OrderListComponent` and `OrderDetailComponent` calling it; one `*.spec.ts`). Type resolution without npm: `tsconfig.json` `paths` to a committed `src/main/webapp/types/` with minimal `.d.ts` stubs for `@angular/core` and `@angular/common/http`.
- Source of truth for the mapping: `~/tools/ts-spike-keep/scip2srctrl.mjs` and `REPORT.md` (mapping table).

**Interfaces:**
- Produces CLI: `node tools/ts-indexer/index.mjs --project <dir> --tsconfig <path> --out <db> [--append]` → exit 0 and a v27 Sourcetrail database containing only TypeScript data (file, node, symbol, edge, source_location, occurrence, local_symbol, error rows); exit 3 with `ts-indexer: node_modules missing in <dir>; run npm install` when type resolution cannot start; prints `ts-indexer: <n> files, <m> errors` on stdout.

- [ ] **Step 1: Write failing `node:test` tests** in `test/convert.test.mjs` against the fixture:
  - `namesRootedAtWebappRoot`: node `app.orders.order.service.OrderService` exists; no name starts with `src.main.webapp` (Review Focus 5).
  - `callVsUsageFromAst`: `OrderListComponent.load()` has a CALL edge to `OrderService.list()`; a method passed as a callback without being invoked is a USAGE, not a CALL.
  - `overloadsAreSeparateNodes`: two overload signatures of `OrderService.find` give two method nodes.
  - `constructorParameterPropertiesAreFields`; `decoratorIsAnnotationUsage` (`OrderListComponent` → `Component`); `moduleCodeGoesToToplevel`.
  - `multiAppLayoutIsReported`: a folder whose root `tsconfig.json` has no `include`/`files` but several app tsconfigs → exit 4 with `ts-indexer: several TypeScript apps in <dir>; not supported yet`.
  - `missingNodeModulesExits3`: on a copy without `types/` paths and without `node_modules`, exit code 3 and the exact message.
- [ ] **Step 2:** `npm ci --prefix tools/ts-indexer && node --test tools/ts-indexer` → FAIL (no implementation).
- [ ] **Step 3:** Port the spike converter into `convert.mjs`/`index.mjs`; replace textual call detection with the TS AST (`CallExpression`/`NewExpression`); split overloads; root names at `baseUrl`/`rootDir`.
- [ ] **Step 4:** Tests pass. Also run on a scratch copy of the user's portal (path from the controller; never commit its output): expect 0 errors and ≥ 4,000 nodes.
- [ ] **Step 5:** Commit (`tools/ts-indexer/`, `testing/cli/angular-spring/`).

### Task 2: TypeScript source-group type with group-scoped clearing

**Files:**
- Modify: `src/lib/settings/source_group/SourceGroupType.h` (add `SOURCE_GROUP_TYPESCRIPT` before `SOURCE_GROUP_CUSTOM_COMMAND`'s neighbours; string `"TypeScript Source Group"`), `src/lib/settings/ProjectSettings.cpp` (load/save).
- Create: `src/lib/settings/source_group/type/SourceGroupSettingsTypeScript.h` (fields: `tsconfigPath` relative to the project folder; trigger patterns fixed: `*.ts`, `*.tsx`, `tsconfig*.json`, `package.json`, `package-lock.json`, `yarn.lock`, `pnpm-lock.yaml`; excludes `node_modules/`, `.sourcetrail/`), `src/lib/project/SourceGroupTypeScript.{h,cpp}` (builds the `IndexerCommandCustom` that runs `node <ts-indexer>/index.mjs --project … --tsconfig … --out %{DATABASE_FILE_PATH}`; `allowsPartialClearing()` true, meaning: any changed trigger file clears and re-imports this group's whole data and nothing else).
- Modify: `src/lib/project/Project.cpp` (`checkIfFilesToClear`, clearing path), `src/lib/data/indexer/TaskExecuteCustomCommands.cpp` (exit 3 → status message `TypeScript skipped: <message>`, not an indexing failure), `src/lib/utility/ResourcePaths*` (path of the shipped ts-indexer: `<app>/data/ts-indexer/index.mjs`; dev builds stage it there from `tools/ts-indexer`).
- Test: `tests/integration/lib/TypeScriptSourceGroupTestSuite.cpp`.

**Interfaces:**
- Consumes: Task 1 CLI and exit codes.
- Produces: `SourceGroupSettingsTypeScript` (`getTsconfigPath()`, `setTsconfigPath(FilePath)`), `SourceGroupTypeScript`, type string `"TypeScript Source Group"` in `.srctrlprj`; the group's data is identifiable for clearing (all files under the group's trigger set).

- [ ] **Step 1: Write failing tests** (fixture copy in a temp dir, headless `Sourcetrail index`):
  - `mixedProjectIndexesBoth`: Java nodes and `app.orders…` TS nodes both present.
  - `javaEditDoesNotRunTypeScript`: touch `OrderService.java`, refresh → the TS indexer is not invoked (its call counter file or log line absent) and TS rows are unchanged.
  - `tsEditReindexesOnlyTypeScript`: edit `order.service.ts` → TS data updated; Java file rows' `indexed` timestamps unchanged.
  - `deletedTsFileIsCleared` (Review Focus 4).
  - `missingNodeModulesSkipsTypeScriptOnly` (Review Focus 1): Java indexed, exit 0, status line `TypeScript skipped: ts-indexer: node_modules missing…`, no TS rows.
  - `javaPartIdenticalToJavaOnlyIndex`: compare-index style comparison of the Java subset vs a Java-only project of the same folder.
- [ ] **Step 2:** Build; run → FAIL.
- [ ] **Step 3:** Implement; stage `tools/ts-indexer` into `bin/app/data/ts-indexer` via a CMake target with `npm ci --omit=dev` (like `java_indexer_jar`).
- [ ] **Step 4:** Full `ctest --preset fork_release` passes; `testing/port/compare-index.sh` on `testing/cli/lombok` still `identical`.
- [ ] **Step 5:** Commit.

### Task 3: Endpoint linking step

**Files:**
- Create: `tools/ts-indexer/link.mjs` (reads the project db; collects TS HTTP call sites with resolved URLs from the TS data written by Task 1 — the converter records each `HttpClient` call's verb and resolved URL in a side table `ts_http_call(source_node_id, verb, url, file_id, line, col)` or an equivalent it owns; parses Spring mappings from the Java files in the index: class `@RequestMapping` + method `@Get/Post/Put/Patch/Delete/RequestMapping(value|path, method)`; normalises path variables to `{}`; writes nodes `HTTP.<VERB> <path>` located at the Spring annotation and CALL edges TS method → endpoint → handler; removes all previous `HTTP.*` nodes/edges first; prints `link: <n> linked, <a> ambiguous, <u> unresolved` and writes the unresolved/ambiguous list to `<project dir>/.st-links.log`), `tools/ts-indexer/test/link.test.mjs`.
- Create: `src/lib/data/indexer/TaskLinkEndpoints.{h,cpp}`; Modify: `src/lib/project/Project.cpp` (append after `TaskFinishParsing` when the refresh re-indexed any TS group file or any Java file containing `@RequestMapping`/`@*Mapping`/`@FeignClient` text, or in a full refresh; skipped when the project has no TypeScript group).
- Modify: `testing/port/compare-index.sh` (new `--exclude-endpoints`).
- Test: `tests/integration/lib/EndpointLinkingTestSuite.cpp`.

**Interfaces:**
- Consumes: Task 1 db rows and the HTTP-call side data; Task 2 group detection.
- Produces: endpoint node names `HTTP.GET /api/orders/{}`; edge pattern TS method -CALL-> endpoint -CALL-> Java handler (later tasks traverse these as ordinary calls).

- [ ] **Step 1: Write failing tests:**
  - node: `linksFixtureCalls`: exactly `HTTP.GET /api/orders`, `HTTP.POST /api/orders`, `HTTP.GET /api/orders/{}` with the expected TS sources and Java handlers (hand-made list in the test).
  - node: `ambiguousHandlerLeavesCallUnlinked` (Review Focus 3): add a second controller mapping `GET /api/orders` → no link for that call, reported ambiguous.
  - node: `unresolvedUrlIsReportedNotGuessed`: a URL built at runtime → unresolved, no link.
  - gtest: `mappingPathEditRelinks` (Review Focus 2): change `@GetMapping("/{id}")` to `"/by-id/{id}"`, refresh → old endpoint gone, new `HTTP.GET /api/orders/by-id/{}` present, frontend call to the old path now unresolved.
  - gtest: `javaOnlyProjectRunsNoLinking`.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement (port `~/tools/ts-spike-keep/link.py` + `endpoints.py` logic to `link.mjs`; one level of constant propagation for URLs as in the spike).
- [ ] **Step 4:** Tests pass; on a scratch copy of the portal: `link: ≥59 linked, 0 ambiguous`, and `compare-index.sh --exclude-endpoints` between a Java-only and a mixed index of the portal copy prints `identical`.
- [ ] **Step 5:** Commit.

### Task 4: `st` across the link

**Files:**
- Modify: `src/st/StCommand.cpp` (options), `src/st/StGraph.cpp` (traversal through endpoint nodes, boundary detection, side filter), `src/st/StFreshness.cpp` (TypeScript trigger files count for staleness; refresh runs Sourcetrail as usual), `src/st/StOutput.cpp`.
- Test: `src/st/tests/StQueryTest.cpp`, `src/st/tests/StTest.cpp`.

**Interfaces:**
- Consumes: endpoint nodes/edges (Task 3), TS nodes (Task 1).
- Produces options:
  - `--boundaries` (callees, impact): output only the boundary methods reached, grouped under `repositories:` and `feign clients:`. A repository is a type that is or extends a Spring Data repository (`org.springframework.data.repository.Repository` subtype) or carries `@Repository`; a Feign client carries `@FeignClient`. Library targets (e.g. `CrudRepository.save`) count and are shown as `OrderRepository.save (CrudRepository)`.
  - `--side frontend|backend` (callers, callees, impact): keep only TypeScript or only Java result lines; with `--side frontend`, `*.spec.ts` callers are dropped unless `--tests`.
  - `--library` (callees, supers, deps): include library targets in default output (default unchanged).

- [ ] **Step 1: Write failing tests** on the fixture index:
  - `calleesBoundariesFromFrontend`: `st callees OrderListComponent.load --depth 0 --boundaries` → exactly `repositories:` `OrderRepository.findAll (JpaRepository)` and `feign clients:` `PaymentClient.authorize`.
  - `callersFrontendSideOfBackend`: `st callers OrderService.list --depth 0 --side frontend` → `OrderService.list` (TS), `OrderListComponent.load`; not the spec; with `--tests` the spec test appears.
  - `stalenessSeesTsEdit`: touching `order.service.ts` makes `st status` report stale; `st` query refreshes it.
  - `javaOnlyOutputUnchanged`: on `testing/cli/lombok`, `callees`/`supers`/`deps` output byte-identical to before this task (golden strings from the current master behaviour).
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `st_test` and full ctest pass; `testing/cli/st-smoke.sh` passes.
- [ ] **Step 5:** Commit.

### Task 5: GUI — TypeScript projects

**Files:**
- Modify: `src/lib/project/JavaProjectSetup.{h,cpp}` → rename to `ProjectSetup.{h,cpp}` (keep the old API as thin wrappers only if callers need it), `src/lib/tests/JavaProjectSetupTestSuite.cpp` → `ProjectSetupTestSuite.cpp`; `src/lib_gui/qt/window/QtMainWindow.cpp` (New Project flow), project settings pages (add/remove the TypeScript group; a TypeScript settings page with the tsconfig path), `QtProjectWizardContentSelect.cpp` (TypeScript type in New Custom Project).

**Interfaces:**
- Produces:
  ```cpp
  struct DetectedBuild { JavaBuildKind java; bool typeScript; };   // typeScript: root tsconfig.json present
  DetectedBuild detectBuild(const FilePath& folder);
  FilePath createProject(const FilePath& folder, const DetectedBuild& build);  // replaces createJavaProject
  ```
  `JavaBuildKind`, `projectFileFor`, `existingProjectFile` unchanged.

- [ ] **Step 1: Write failing tests:** `detectsTypeScriptNextToGradle`, `detectsTypeScriptOnly`, `noneWhenNeitherExists`, `createWritesBothGroups` (loads back with a Gradle and a TypeScript group), `createTypeScriptOnlyProject` (one TypeScript group, no Java group), existing Java-only detection tests unchanged.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement; wire the GUI: the "no Gradle or Maven build found" message now appears only when neither build nor `tsconfig.json` exists.
- [ ] **Step 4:** Tests pass; under `xvfb-run` with a scratch HOME: New Project on a copy of `testing/cli/angular-spring` creates one project with both groups and indexes with 0 errors; on a copy of only its `src/main/webapp` creates a TypeScript-only project; project settings can remove and re-add the TypeScript group.
- [ ] **Step 5:** Commit.

### Task 6: CI, packaging and docs

**Files:**
- Modify: `.github/workflows/build.yml` (`actions/setup-node` Node 22; `npm ci` for `tools/ts-indexer`; `node --test tools/ts-indexer`), `setup/Linux/createPackages.sh` (ship `data/ts-indexer` with its production `node_modules`), `testing/cli/st-smoke.sh` (a TypeScript section on the angular-spring fixture: index, `callees --boundaries`, `callers --side frontend`), `README.md` (TypeScript: prerequisites Node + `npm install`, the two questions, the new flags, what is not linked), `docs/agents/st-claude-md-snippet.md` (question→command rows for both directions).
- Create: `docs/superpowers/specs/2026-11-03-typescript-indexing-results.md` (acceptance checks 1–5 from the spec with numbers on the fixture and on the portal copy — no private names: "a private Angular + Spring app").

- [ ] **Step 1:** CI green on a pushed branch (ask the user before pushing).
- [ ] **Step 2:** Package check in a clean ubuntu:22.04 container with Node 22 installed: packaged `st` indexes the fixture copy and answers both questions; without Node: Java part indexes, one "TypeScript skipped" line.
- [ ] **Step 3:** Commit docs and results.
