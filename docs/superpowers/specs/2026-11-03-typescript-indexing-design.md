# TypeScript indexing and the frontend–backend link

Date: 2026-11-03. Status: draft for approval.

## Why

The user's stack is an Angular/TypeScript frontend on a Java Spring backend. Today Sourcetrail and `st` only index the
Java side, so the two questions that matter most when changing such an app cross a blind spot:

1. **Downstream from the frontend:** "I'm at this Angular method. What does it affect if it's invoked?" The answer
   follows the call through the frontend services, across the HTTP request into the Spring endpoint, through the
   backend, and ends at the **edges** it reaches: the repositories it touches and the Feign clients it calls.
2. **Upstream from the backend:** "I'm changing this Java service method. Who on the frontend side reaches it?" The
   answer is the Angular services and components that end up calling it.

A throwaway spike (report kept at `~/tools/ts-spike-keep/REPORT.md`, scripts beside it) proved the approach on the
user's portal app (327 TypeScript files, Angular + Spring in one folder): `scip-typescript` plus a ~450-line converter
writes nodes, edges and locations into the Sourcetrail database; a small linking pass joins 59 of 68 frontend HTTP calls
to their Spring handlers with none ambiguous; `st` and the GUI then answer across both languages. Indexing took about
5 s from scratch.

Success: on the portal, both questions above are answered by one `st` command each, correctly, without the agent
reading source to bridge the HTTP gap.

## Goals

1. **TypeScript in the index.** A project folder that contains a `tsconfig.json` gets its TypeScript indexed into the
   same Sourcetrail database as its Java code: modules, classes, interfaces, functions, methods, fields, and call,
   usage, type-usage, inheritance, override, import and decorator links, with source locations.
2. **The HTTP link.** Each frontend HTTP call (`HttpClient.get/post/put/patch/delete` with a URL the analysis can
   resolve) is linked to the Spring handler with the same verb and path, through an endpoint node, so call chains run
   frontend → endpoint → handler and back.
3. **Both questions answered by `st`'s existing commands across the link:** `callees`/`impact` downstream from a
   TypeScript method, `callers`/`impact` upstream from a Java method.
4. **Answers that stay short:**
   - Downstream: the answer can be limited to the edges reached, i.e. repository methods (Spring Data repositories)
     and Feign client methods, grouped by type, instead of every method in between. The full path stays available.
   - Upstream: the answer can be limited to the frontend side (TypeScript callers only).
   - (My call on the exact flag names; see "Left to the builder".)
5. **Seamless setup, and TypeScript projects in the GUI.** The user asked that the UI allow adding a TypeScript
   project. In the new GUI project flow (pick a folder):
   - a folder with a `tsconfig.json` **and** a Gradle/Maven build gets both, in one project, linked;
   - a folder with a `tsconfig.json` and **no** Java build (a frontend in its own repo) becomes a TypeScript-only
     project — instead of the current "no Gradle or Maven build found" message, which then only appears when neither
     exists;
   - existing projects can add (or remove) the TypeScript part in their project settings, and the GUI offers it when
     a `tsconfig.json` is present;
   - New Custom Project (the old wizard) offers TypeScript as a source-group type too.
   It can be switched off per project. A TypeScript-only project has no HTTP link (there is no backend in the index).
6. **Cheap refresh.** The TypeScript step re-runs only when TypeScript files (or `tsconfig*.json`, `package.json`)
   changed. A Java-only edit must not run it, so Java refresh times stay as they are. On the portal a TypeScript
   refresh costs about 5 s.

## Non-goals

- **Angular HTML templates.** Calls from templates (`(click)="pause()"`, bindings, child components, pipes) are not
  indexed in this phase. Neither question needs them: downstream never goes through a template, and upstream ends at
  the component class, which calls the Angular service itself. A template pass is a later, separate step.
- **Routes/URLs of views.** Upstream answers end at components, not at the Angular route a component is shown on.
- **Across indexes.** A frontend in a different folder or repo than its backend gets its own index and no link. Feign
  clients are edges: the trail never follows into another service, even if that service is indexed elsewhere.
- **Python.** It comes next, as its own piece of work.
- **JavaScript-only projects** (no `tsconfig.json`), Vue/React specifics, and other backends than Spring.
- **The C++ indexer, Windows, macOS** — unchanged from the fork's scope.

## Anti-goals (failure even if it works)

- Java refresh gets slower because the TypeScript step runs on Java-only edits.
- A wrong HTTP link: a frontend call joined to the wrong handler. An unresolved or ambiguous URL must produce no link
  (and be reported), never a guess.
- The Java part of the index changes because TypeScript was added. Check: `testing/port/compare-index.sh` on a
  Java-only project gives identical results with the feature on and off; on a mixed project the Java nodes, edges and
  locations are identical to a Java-only index of the same folder.
- `st`'s existing Java answers get noisier: with no TypeScript in a project, every command's output is unchanged.
- Private project, package or class names in committed files (the repo is public). Tests use a neutral fixture.

## Constraints

- Build on the fork as it is now: master at v2.0.0-fork (Conan 2, Qt 6.8, C++20), index storage version 27, `st`
  in `src/st`, the `.sourcetrail/` project folder, CI that builds and packages on Ubuntu 22.04.
- Indexing TypeScript needs Node.js and the project's `node_modules` (installed by the user's normal `npm install`);
  `scip-typescript` resolves types through them. If Node or `node_modules` is missing, the project still indexes its
  Java, and the TypeScript step reports one clear error instead of failing the whole refresh.
- The TypeScript part plugs in through the existing **Custom Command source group** (the command writes its own
  database, which Sourcetrail merges into the project's index — `TaskExecuteCustomCommands`), so the C++ core needs
  no new language package. The spike showed this works, including refresh.
- Spring mapping paths (`@RequestMapping` on the class plus `@GetMapping` etc. on the method) are not stored in the
  index today; the linking pass reads them from the Java source (or the Java indexer is extended to record them —
  builder's choice, see below).
- Every commit builds and passes the tests; tests come with or before the code; follow the repo's formatting and the
  harness style for anything under `testing/`.

## Decisions from the builder check (my calls, marked as such)

1. **Linking is its own cheap step, separate from the TypeScript step.** The endpoint nodes and their links sit in
   Java files (at the Spring mapping) and TS files (at the call), so re-indexing either side clears them. The linking
   pass therefore runs after every refresh that re-indexed a TypeScript file or a Java file containing a Spring
   mapping or Feign/HTTP annotation, in both the GUI and `st` refresh paths. It reads only the index plus those
   files' mapping annotations, so it costs well under the TypeScript step. "A Java-only edit must not run the
   TypeScript step" means `scip-typescript` + converter; the linking pass may run. (My call.)
2. **Core changes are allowed.** This is the fork's own code. The Custom Command group today does not allow partial
   clearing (`SourceGroupCustomCommand::allowsPartialClearing()` is false), so a `.ts` edit would force a full
   re-index of the Java. The builder adds group-scoped clearing so a TypeScript change clears and re-imports only the
   TypeScript group's data, and defines the TypeScript group's trigger files itself (`*.ts`, `*.tsx`,
   `tsconfig*.json`, `package.json`, the lockfile) instead of relying on the single `tsconfig.json` source file.
   (My call.)
3. **The Java identity check excludes the endpoint layer.** "Java nodes, edges and locations identical" is checked
   with the `HTTP.*` endpoint nodes and every link to or from them excluded; everything else in Java files must be
   identical to a Java-only index. `testing/port/compare-index.sh` gets an option for that. (My call.)
4. **`st` handles TypeScript freshness, the GUI creates the project.** Projects are still created in the GUI (as
   today). `st` must treat the TypeScript group as part of the project: detect stale TypeScript (trigger files
   above), run the refresh, and include it in `st status`/freshness. `st`'s project registration does not need to set
   up TypeScript. (My call.)
5. **One TypeScript project per folder.** The root `tsconfig.json` (with its `references`) defines it; the webapp
   root is taken from the tsconfig's `baseUrl`/`rootDir` rather than a hard-coded prefix. Multi-app layouts (several
   independent tsconfigs, Nx) are out of scope for this phase and reported as such. Spec files are indexed, but the
   frontend-side filter for question 2 leaves `*.spec.ts` callers out by default, with a flag to include them.
   (My call.)
6. **No config-based URL prefixes.** Server prefixes from configuration (`server.servlet.context-path`, a gateway
   prefix, a frontend base-URL constant beyond one level of constant propagation) are not resolved; such calls stay
   unlinked and are listed as unresolved. The spike's portal links needed none of them (68/68 resolved, 59 matched).
   (My call.)
7. **Library targets.** `userRepository.save(...)` resolves to a library method (`CrudRepository.save`), so the
   edges filter must include library targets. The default output of `callees`/`supers`/`deps` stays as today; library
   targets appear in the edges-filtered answer and behind an explicit flag only. (My call.)

## The spike's design, to carry over

From `~/tools/ts-spike-keep/REPORT.md` (mapping table, link counts, gaps):
- Names use `.` as delimiter: directories → packages, each file → a module named after its stem, each npm package →
  one package node. Signatures come from SCIP hover text.
- Which declaration a reference belongs to: the innermost declaration from `enclosing_range`, filled in by a
  syntax-only TypeScript parse (only about a sixth of definitions carry `enclosing_range`). Module-level code goes to
  a synthetic `<module>.toplevel()` because `st` drops links whose source is a module.
- Call vs usage: the spike decided textually (callable followed by `(`); production should use the AST
  (`CallExpression`/`NewExpression`).
- Endpoint nodes: `HTTP.<VERB> <path>` with path variables normalised to `{}`, declared at the Spring annotation;
  links TS method -call-> endpoint -call-> Java handler. URL resolution: string literals, template strings,
  `getEndpointFor('api/…')`-style helpers, `resourceUrl` fields — one level of constant propagation sufficed on the
  portal (68/68 resolved).

Known spike gaps that this work must fix: AST-based call detection; overloads merged; re-exports only as imports;
`st` hiding library targets in `callees`/`supers`/`deps` (affects Java too).

## Acceptance checks

On a neutral Angular + Spring fixture committed to the repo (a small app in one folder) and on a scratch copy of the
user's portal:
1. `st callees <frontend method> --depth N` with the edges filter lists the repositories and Feign clients the
   method reaches, grouped, and nothing else.
2. `st callers <backend service method>` with the frontend filter lists the Angular services and components that
   reach it.
3. On the portal, the HTTP link count is at least the spike's (59 linked, 0 ambiguous, 0 wrong — checked against a
   hand-made list for the fixture).
4. A Java-only edit's refresh does not run the TypeScript step (visible in the refresh log and timing) and does not
   re-index any TypeScript; a `.ts` edit re-indexes only the TypeScript group, not the Java. After either edit,
   including an edit to a controller's mapping path, the HTTP links are correct (re-created, never stale).
5. The anti-goal checks above pass.

## Left to the builder

- Converter language and packaging: keep the spike's Node script (it needs Node anyway for `scip-typescript`) or
  port it; how `scip-typescript` is shipped or installed (bundled in the package, or `npx` on first use).
- Whether Spring mapping paths come from parsing Java source in the linking pass or from extending the Java indexer.
- Exact `st` flag names for the two filters, and how the GUI shows endpoint nodes.
- Whether a TypeScript refresh is incremental or re-runs the whole program (whole program is fine at the portal's
  size).
