# TypeScript indexing and the frontend–backend link: results

Measured on Nov 3–4, 2026, on two projects:
- **The fixture**, `testing/cli/angular-spring`: a small Angular + Spring shop in one folder (Gradle, 16 Java files,
  9 TypeScript files, stand-ins for Spring and Feign).
- **A private Angular + Spring app** (Maven, 327 TypeScript files, 429 indexed files), on a scratch copy with its own
  `node_modules`. Counts only, no names.

Spec: `2026-11-03-typescript-indexing-design.md`. Acceptance checks 1–5 are below, followed by the Java timing, the
package check and CI.

## 1. Downstream: `st callees <frontend method> --boundaries`

| Project | Method | `callees --depth 0` | `--boundaries` |
|---|---|---|---|
| Fixture | `OrderListComponent.load` | 6 lines | `repositories:` `OrderRepository.findAll (JpaRepository)` |
| Fixture | TypeScript `OrderService.create` | | `repositories:` `OrderRepository.save (JpaRepository)`; `feign clients:` `PaymentClient.authorize` |
| Private app | a list component's `load()` | 15 lines | 1 line, a repository method named after its repository |
| Private app | the widest method (a dashboard component's save) | 134 lines | 15 lines in 0.6 s: 13 repository-own query methods, 2 inherited Spring Data methods named after their repository |

Only repository and Feign client methods are listed, grouped under their titles. The private app has no Feign clients,
so that group was checked on the fixture only. Tests: `StAcrossTheLink.calleesBoundariesFromFrontend` and the
`boundaries...` cases in `StQueryTest.cpp`.

## 2. Upstream: `st callers <backend method> --side frontend`

| Project | Method | `callers --depth 0` | `--side frontend` | `--side frontend --tests` | `--side backend` |
|---|---|---|---|---|---|
| Fixture | `com.acme.shop.OrderService.list` | 11 | 8 (the Angular services and components) | 9 (adds the spec file's top level) | 1 (`OrderResource.list`) |
| Private app | a domain service's `findAll()` | 15 | 9 | 13 | 1 (the REST resource method) |

`impact --side frontend` on the private app: 15 symbols in 9 files without the filter, 9 in 4 files with it. Test:
`StAcrossTheLink.callersFrontendSideOfBackend`.

## 3. HTTP links

| Project | Linked | Ambiguous | Unresolved | Spike |
|---|---|---|---|---|
| Fixture | 3 | 0 | 1 | |
| Private app | 72 | 0 | 9 | 59 linked, 0 ambiguous |

- Fixture: the three linked calls and the one unresolved call (`http.get(location)`, a URL passed in as a parameter)
  match the hand-made list in `EndpointLinking.linksFixtureCalls` and the linker's `linksFixtureCalls` test.
- Private app: 81 frontend calls, all with a resolved URL. The 9 unresolved calls have no handler: 8 go to Spring Boot
  management endpoints and 1 to a path whose only handler is under another prefix. Each was checked by hand; none is a
  missed link. 97 endpoints, 67 of them with frontend callers.
- **Wrong links: none found.** An automated cross-check of both sides: each resolved URL's literal path segments
  appear in its TypeScript file (32 on the call itself, 49 through a constant in the same file), and each endpoint's
  path segments appear in the Java file where the endpoint is declared (97 of 97). An ambiguous match is never linked
  (`ambiguousHandlerLeavesCallUnlinked`).

## 4. Refresh

| Edit | Project | Re-indexed | TypeScript step | Linking | Total |
|---|---|---|---|---|---|
| A Java service method | Fixture | 1 Java file | not run | not run | 3.1 s |
| A `.ts` file | Fixture | TypeScript group only (9 files) | ran | 3 / 0 / 1 | 2.0 s |
| A controller's mapping | Private app | 1 Java file | not run | 72 / 0 / 9 | |
| A `.ts` file | Private app | TypeScript group only (327 files), 0 Java files | 3.5 s | 0.3 s | 10.1 s |

- The links are correct after every kind of edit: a mapping path edit drops the old link and creates the new one, with
  no stale `HTTP.*` node (`EndpointLinking.mappingPathEditRelinks`); a deleted `.ts` file takes its nodes, calls and links
  with it and leaves the Java untouched (`TypeScriptSourceGroup.deletedTsFileIsCleared`); also
  `javaEditDoesNotRunTypeScript`, `tsEditReindexesOnlyTypeScript`, `configEditReindexesOnlyTypeScript`,
  `typeScriptEditRelinksWithoutTouchingJava` and `deletedControllerDoesNotRunTypeScript`.
- **Known gap:** on a Maven project, a TypeScript-only refresh takes about 10 s, not the spec's 5 s. The TypeScript step
  itself takes 3–5 s (3.5 s above); the rest is Maven's preparation (generate-sources, dependency copy, effective POM),
  which runs on every refresh and is not cached the way Gradle's is. This is not new with this work. Follow-up: a Maven
  preparation cache keyed on `pom.xml`, `.mvn/` and `settings.xml`, with generate-sources only on a full refresh or a
  key change.

## 5. Anti-goals

| Anti-goal | Result |
|---|---|
| Java refresh slower because the TypeScript step runs on Java edits | It never runs on a Java-only edit (table above). The Java indexer itself is as fast as before: see "Java timing" below. |
| A wrong HTTP link | None found (section 3). Unresolved and ambiguous calls get no link and are counted. |
| The Java part of the index changes | `compare-index.sh` on the lombok fixture: identical with the feature on and off. Private app, Java-only against mixed index with `--exclude-endpoints`: `identical (217625 rows)`. Test: `TypeScriptSourceGroup.javaPartIdenticalToJavaOnlyIndex`. |
| `st` answers noisier without TypeScript | `StIndex.javaOnlyOutputUnchanged`: 8 `callees`/`supers`/`deps` commands on the lombok fixture print byte for byte what the pre-change binary printed; `st status` has no `typescript` row. |
| Private names in committed files | None: the branch diff against master was searched for the private app's names. |

Missing Node or `node_modules`: the Java indexes and one `TypeScript skipped: ...` line says why
(`missingNodeSkipsTypeScriptOnly`, `missingNodeModulesSkipsTypeScriptOnly`, and the package check below).

## Java timing

The branch guards the Java parser registry with a mutex, taken on every callback from the Java indexer (a crash fix,
commit `c8d0eea81`). Goal 6 asks for unchanged Java times, so the branch build was compared with the pre-TypeScript
build of master (the same Java code without the mutex). Project: `testing/benchmark`'s realworld app (116 Java files,
Gradle 7.4 on JDK 17, Lombok pinned to 1.18.30 in both copies because 1.18.22 cannot delombok under the JDK 21 the
indexer runs on), indexer threads automatic on a 20-core machine, warm Gradle daemon. Runs interleaved; medians.

| | Branch | Pre-TypeScript build |
|---|---|---|
| `Sourcetrail index --full`, wall (10 runs each) | 8.02 s | 8.02 s |
| Same, indexing time the indexer reports | 4.10 s | 4.11 s |
| Same, CPU time user (5 runs each) | 25.0 s | 25.8 s |
| Refresh after a one-line edit, wall (6 runs each) | 3.02 s | 3.02 s |
| Same, indexing time the indexer reports | 1.36 s | 1.32 s |
| Same, CPU time user | 4.16 s | 4.09 s |

No difference above the measurement's resolution: wall times step in whole seconds (the headless indexer polls), the
reported indexing times in steps of about 25 ms, and the spread between runs of the same build is larger than the gap
between the builds. Both builds indexed the same 1,799 nodes. The full index is not 5% slower, so the mutex stays.

## Package (Ubuntu 22.04)

Built, tested and packaged in an Ubuntu 22.04 container with Node 22 (the CI steps): `ctest` 984 of 984 passed,
`node --test tools/ts-indexer` 53 of 53, the package (71 MB) holds `share/data/ts-indexer` with its production
`node_modules`, and `st-smoke.sh` on the packaged `st` passed, including its new TypeScript section.

Then the package alone, in a clean `ubuntu:22.04` with only the GUI's system libraries, a JDK 21 and Node 22.23.4:

| | With Node | Without Node |
|---|---|---|
| `st index --full` on a fixture copy | `indexed 17 files, 0 errors`, `link: 3 linked, 0 ambiguous, 1 unresolved` | `indexed 17 files, 0 errors`, one `TypeScript skipped: cannot run "node": No such file or directory` |
| `st status` | `typescript indexed` | `typescript not indexed; st index shows why`, 16 Java files indexed |
| `callees OrderListComponent.load --depth 0 --boundaries` | `OrderRepository.findAll (JpaRepository)` | |
| `callers com.acme.shop.OrderService.list --depth 0 --side frontend` | the 8 Angular methods | |
| `callers com.acme.shop.OrderService.list --depth 0` | | `OrderResource.list()`: the Java half answers |

Without Node the linking step also says `Endpoint linking skipped: cannot run "node": ...`.

## CI

`.github/workflows/build.yml` sets up Node 22, so CMake stages the TypeScript indexer and the TypeScript tests in
`ctest` run; a new step runs `npm ci` and `node --test tools/ts-indexer`; the packaged smoke test now indexes the
fixture and asks both questions. The workflow was checked locally (the YAML parses; its steps reproduced in the
Ubuntu 22.04 container above); a run on GitHub is still to come with the first push of the branch.
